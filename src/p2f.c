/*
 *	
 * Copyright (c) 2016 Cisco Systems, Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 *   Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * 
 *   Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer in the documentation and/or other materials provided
 *   with the distribution.
 * 
 *   Neither the name of the Cisco Systems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * p2f.c
 *
 * converts pcap files or live packet captures using libpcap into
 * flow/intraflow data in JSON format
 *
 * this file contains the functions relating to flow_records,
 * flow_keys, and the management of the flow cache
 */

#include <stdlib.h>   
#include <unistd.h>    /* for fork()                     */
#include <sys/types.h> /* for waitpid()                  */
#include <sys/wait.h>  /* for waitpid()                  */
#include "pkt_proc.h" /* packet processing               */
#include "p2f.h"      /* pcap2flow data structures       */
#include "err.h"      /* error codes and error reporting */
#include "anon.h"     /* address anonymization           */
#include "tls.h"      /* TLS awareness                   */
#include "classify.h" /* inline classification           */
#include "wht.h"      /* walsh-hadamard transform        */
#include "procwatch.h"  /* process to flow mapping       */
#include "radix_trie.h" /* trie for subnet labels        */
#include "config.h"     /* configuration                 */

/*
 * for portability and static analysis, we define our own timer
 * comparison functions (rather than use non-standard
 * timercmp/timersub macros)
 */

inline unsigned int timer_gt(const struct timeval *a, const struct timeval *b) {
  return (a->tv_sec == b->tv_sec) ? (a->tv_usec > b->tv_usec) : (a->tv_sec > b->tv_sec);
}

inline unsigned int timer_lt(const struct timeval *a, const struct timeval *b) {
  return (a->tv_sec == b->tv_sec) ? (a->tv_usec < b->tv_usec) : (a->tv_sec < b->tv_sec);
}

inline void timer_sub(const struct timeval *a, const struct timeval *b, struct timeval *result)  {  
  result->tv_sec = a->tv_sec - b->tv_sec;        
  result->tv_usec = a->tv_usec - b->tv_usec;     
  if (result->tv_usec < 0) {                         
    --result->tv_sec;                                
    result->tv_usec += 1000000;                      
  }                                                    
}

inline void timer_clear(struct timeval *a) { 
  a->tv_sec = a->tv_usec = 0; 
}


/*
 * The VERSION variable should be set by a compiler directive, based
 * on the file with the same name.  This value is reported in the
 * "metadata" object in JSON output.
 */

#ifndef VERSION
#define VERSION "unknown"
#endif

/*
 *  global variables
 */

radix_trie_t rt = NULL;

enum SALT_algorithm salt_algo = raw;

enum print_level output_level = none;

struct flocap_stats stats = {  0, 0, 0, 0 };
struct flocap_stats last_stats = { 0, 0, 0, 0 };
struct timeval last_stats_output_time;

unsigned int num_pkt_len = NUM_PKT_LEN;

void convert_string_to_printable(char *s, unsigned int len);


#include "osdetect.h"


/* START flow monitoring */


unsigned int timeval_to_milliseconds(struct timeval ts) {
  unsigned int result = ts.tv_usec / 1000 + ts.tv_sec * 1000;
  return result;
}

void flocap_stats_output(FILE *f) {
  char time_str[128];
  // time_t now = time(NULL);
  struct timeval now, tmp;
  float bps, pps, rps, seconds;

  gettimeofday(&now, NULL);
  timer_sub(&now, &last_stats_output_time, &tmp);
  seconds = (float) timeval_to_milliseconds(tmp) / 1000.0;
  bps = (float) (stats.num_bytes - last_stats.num_bytes) / seconds;
  pps = (float) (stats.num_packets - last_stats.num_packets) / seconds;
  rps = (float) (stats.num_records_output - last_stats.num_records_output) / seconds;

  strftime(time_str, sizeof(time_str)-1, "%a %b %2d %H:%M:%S %Z %Y", localtime(&now.tv_sec));
  fprintf(f, "%s info: %lu packets, %lu active records, %lu records output, %lu alloc fails, %.4e bytes/sec, %.4e packets/sec, %.4e records/sec\n", 
	  time_str, stats.num_packets, stats.num_records_in_table, stats.num_records_output, stats.malloc_fail, bps, pps, rps);
  fflush(f);

  last_stats_output_time = now;
  last_stats = stats;
}

void flocap_stats_timer_init() {
  struct timeval now;

  gettimeofday(&now, NULL);
  last_stats_output_time = now;
}


/* configuration state */

unsigned int bidir = 0;

unsigned int include_zeroes = 0;

unsigned int byte_distribution = 0;

unsigned int report_entropy = 0;

unsigned int report_wht = 0;

unsigned int report_idp = 0;

unsigned int report_hd = 0;

unsigned int report_dns = 0;

unsigned int include_tls = 0;

unsigned int include_classifier = 0;

unsigned int nfv9_capture_port = 0;

FILE *output = NULL;

FILE *info = NULL;

unsigned int records_in_file = 0;

/*
 * config is the global configuration 
 */
struct configuration config = { 0, };


/*
 * by default, we use a 10-second flow inactivity timeout window
 * and a 30-second activity timeout; the active_timeout represents
 * the difference between those two times
 */
#define T_WINDOW 10
#define T_ACTIVE 20

struct timeval time_window = { T_WINDOW, 0 };

struct timeval active_timeout = { T_ACTIVE, 0 };

unsigned int active_max = (T_WINDOW + T_ACTIVE);

int include_os = 1;

#define expiration_type_reserved 'z'
#define expiration_type_active  'a'
#define expiration_type_inactive 'i'


#define flow_key_hash_mask 0x000fffff
// #define flow_key_hash_mask 0xff

#define FLOW_RECORD_LIST_LEN (flow_key_hash_mask + 1)

typedef struct flow_record *flow_record_list;

flow_record_list flow_record_list_array[FLOW_RECORD_LIST_LEN] = { 0, };

enum twins_match { exact = 0, near = 1 };

// enum twins_match flow_key_match_method = exact;

unsigned int flow_key_hash(const struct flow_key *f) {

  if (config.flow_key_match_method == exact) {
    return (((unsigned int)f->sa.s_addr * 0xef6e15aa) 
	    ^ ((unsigned int)f->da.s_addr * 0x65cd52a0) 
	    ^ ((unsigned int)f->sp * 0x8216) 
	    ^ ((unsigned int)f->dp * 0xdda37) 
	    ^ ((unsigned int)f->prot * 0xbc06)) & flow_key_hash_mask;

  } else {  /* flow_key_match_method == near */
    /*
     * To make it possible to identify NAT'ed twins, the hash of the
     * flows (sa, da, sp, dp, pr) and (*, *, dp, sp, pr) are identical.
     * This is done by omitting addresses and sorting the ports into
     * order before hashing.
     */
    unsigned int hi, lo;  
    
    if (f->sp > f->dp) {
      hi = f->sp;
      lo = f->dp;
    } else {
      hi = f->dp;
      lo = f->sp;
    }
    
    return ((hi * 0x8216) ^ (lo * 0xdda37) 
	    ^ ((unsigned int)f->prot * 0xbc06)) & flow_key_hash_mask;
  }
}

struct flow_record *flow_record_chrono_first = NULL;
struct flow_record *flow_record_chrono_last = NULL;

void flow_record_list_init() {
  unsigned int i;
  
  flow_record_chrono_first = flow_record_chrono_last = NULL;
  for (i=0; i<FLOW_RECORD_LIST_LEN; i++) {
    flow_record_list_array[i] = NULL; 
  }
}

void flow_record_list_free() {
  struct flow_record *record, *tmp;
  unsigned int i, count = 0;

  for (i=0; i<FLOW_RECORD_LIST_LEN; i++) {
    record = flow_record_list_array[i];
    while (record != NULL) {
      tmp = record->next;
      // fprintf(stderr, "freeing record %p\n", record);
      flow_record_delete(record);
      record = tmp;
      count++;
    }
    flow_record_list_array[i] = NULL;
  }
  flow_record_chrono_first = NULL;
  flow_record_chrono_last = NULL;

  // fprintf(output, "freed %u flow records\n", count);
}


int flow_key_is_eq(const struct flow_key *a, const struct flow_key *b) {
  //return (memcmp(a, b, sizeof(struct flow_key)));
  // more robust way of checking keys are equal
  //   0: flow keys are equal
  //   1: flow keys are not equal
  if (a->sa.s_addr != b->sa.s_addr) {
    return 1;
  }
  if (a->da.s_addr != b->da.s_addr) {
    return 1;
  }
  if (a->sp != b->sp) {
    return 1;
  }
  if (a->dp != b->dp) {
    return 1;
  }
  if (a->prot != b->prot) {
    return 1;
  }

  // match was found
  return 0;
}

int flow_key_is_twin(const struct flow_key *a, const struct flow_key *b) {
  //return (memcmp(a, b, sizeof(struct flow_key)));
  // more robust way of checking keys are equal
  //   0: flow keys are equal
  //   1: flow keys are not equal
  if (config.flow_key_match_method == near) {
    /* 
     * Require that only one address match, so that we can find twins
     * even in the presence of NAT; that is, (sa, da) equals either
     * (*, sa) or (da, *).
     *
     * Note that this scheme works only with Network Address
     * Translation (NAT), and not Port Address Translation (PAT).  NAT
     * is commonly done with and without PAT.
     */ 
    if (a->sa.s_addr != b->da.s_addr && a->da.s_addr != b->sa.s_addr) {
      return 1;
    }
  } else {
    /*
     * require that both addresses match, that is, (sa, da) == (da, sa)
     */
    if (a->sa.s_addr != b->da.s_addr) {
      return 1;
    }
    if (a->da.s_addr != b->sa.s_addr) {
      return 1;
    }
  }
  if (a->sp != b->dp) {
    return 1;
  }
  if (a->dp != b->sp) {
    return 1;
  }
  if (a->prot != b->prot) {
    return 1;
  }

  // match was found
  return 0;
}

void flow_key_copy(struct flow_key *dst, const struct flow_key *src) {
  dst->sa.s_addr = src->sa.s_addr;
  dst->da.s_addr = src->da.s_addr;
  dst->sp = src->sp;
  dst->dp = src->dp;
  dst->prot = src->prot;
}

#define MAX_TTL 255

struct flow_record *flow_key_get_twin(const struct flow_key *key);

void flow_key_print(const struct flow_key *key);

void flow_record_init(/* @out@ */ struct flow_record *record, 
		      /* @in@  */ const struct flow_key *key) {

  flocap_stats_incr_records_in_table();
 
  flow_key_copy(&record->key, key); 
  record->np = 0;
  record->op = 0;
  record->ob = 0;
  record->num_bytes = 0;
  record->bd_mean = 0.0;
  record->bd_variance = 0.0;
  record->seq = 0;
  record->ack = 0;
  record->invalid = 0;
  record->retrans = 0;
  record->ttl = MAX_TTL;
  timer_clear(&record->start);
  timer_clear(&record->end);
  record->last_pkt_len = 0;
  memset(record->byte_count, 0, sizeof(record->byte_count));
  memset(record->pkt_len, 0, sizeof(record->pkt_len));
  memset(record->pkt_time, 0, sizeof(record->pkt_time));
  memset(record->pkt_flags, 0, sizeof(record->pkt_flags));
  record->exe_name = NULL;
  record->tcp_option_nop = 0;
  record->tcp_option_mss = 0;
  record->tcp_option_wscale = 0;
  record->tcp_option_sack = 0;
  record->tcp_option_tstamp = 0;
  record->tcp_initial_window_size = 0;
  record->tcp_syn_size = 0;
  memset(record->dns_name, 0, sizeof(record->dns_name));
  record->idp = NULL;
  record->idp_len = 0;
  record->exp_type = 0;
  record->first_switched_found = 0;
  record->next = NULL;
  record->prev = NULL;
  record->time_prev = NULL;
  record->time_next = NULL;
  record->twin = NULL;

  /* initialize TLS data */
  tls_record_init(&record->tls_info);

  wht_init(&record->wht);

  header_description_init(&record->hd);

#ifdef END_TIME
  record->end_time_next = NULL;
  record->end_time_prev = NULL;
#endif
}

struct flow_record *flow_record_list_find_record_by_key(const flow_record_list *list, 
							const struct flow_key *key) {
  struct flow_record *record = *list;

  /* find a record matching the flow key, if it exists */
  while (record != NULL) {
    if (flow_key_is_eq(key, &record->key) == 0) {
      debug_printf("LIST (head location: %p) record %p found\n", list, record);
      return record;
    }
    record = record->next;
  }
  debug_printf("LIST (head location: %p) did not find record\n", list);  
  return NULL;
}

struct flow_record *flow_record_list_find_twin_by_key(const flow_record_list *list, 
						      const struct flow_key *key) {
  struct flow_record *record = *list;

  /* find a record matching the flow key, if it exists */
  while (record != NULL) {
    if (flow_key_is_twin(key, &record->key) == 0) {
      debug_printf("LIST (head location: %p) record %p found\n", list, record);
      return record;
    }
    record = record->next;
  }
  debug_printf("LIST (head location: %p) did not find record\n", list);  
  return NULL;
}

void flow_record_list_prepend(flow_record_list *head, struct flow_record *record) {
  struct flow_record *tmp;

  tmp = *head;
  if (tmp == record) {
    printf("setting record->next to record! (%p)\n", record);
  }
  if (tmp != NULL) {
    tmp->prev = record;
    record->next = tmp;
  }
  *head = record;
  debug_printf("LIST (head location %p) head set to %p (prev: %p, next: %p)\n", 
	       head, *head, record->prev, record->next); 

}


unsigned int flow_record_list_remove(flow_record_list *head, struct flow_record *r) {
  
  if (r == NULL) {
    return 1;    /* don't process NULL pointers; probably an error to get here */
  }

  debug_printf("LIST (head location %p) removing record at %p (prev: %p, next: %p)\n", 
	       head, r, r->prev, r->next); 

  if (r->prev == NULL) {
    /*
     * r is the first (or only) record within its flow_record_list, so
     * the head of the list must be set
     */
    if (*head != r) {
      printf("error: frla[hk] != r\n");
      exit(1);
    }
    *head = r->next;
    if (*head != NULL) {
      /* 
       * the list is not empty, so we need to set the prev pointer in
       * the first record to indicate that it is the head of the list 
       */
      (*head)->prev = NULL;  
    }
  } else {
    /* 
     * r needs to be stitched out of its flow_record_list, by setting
     * its previous entry to point to its next entry
     */
    r->prev->next = r->next;
    debug_printf("LIST (head location %p) now prev->next: %p\n", head, r->prev->next); 
    if (r->next != NULL) {
      /*
       * the next entry's previous pointer must be made to point to
       * the previous entry 
       */
      r->next->prev = r->prev;
      debug_printf("LIST (head location %p) now next->prev: %p\n", head, r->next->prev); 
    }
  }

  return 0; /* indicate success */
}

void flow_record_list_unit_test() {
  flow_record_list list;
  struct flow_record a, b, c, d;
  struct flow_record *rp;
  struct flow_key k1 = { { 0xcafe }, { 0xbabe }, 0xfa, 0xce, 0xdd };
  struct flow_key k2 = { { 0xdead }, { 0xbeef }, 0xfa, 0xce, 0xdd };

  flow_record_init(&a, &k1);
  flow_record_init(&b, &k2);
  flow_record_init(&c, &k1);
  flow_record_init(&d, &k1);

  list = NULL;  /* initialization */
  flow_record_list_prepend(&list, &a);
  rp = flow_record_list_find_record_by_key(&list, &k1);
  if (rp) {
    printf("found a\n");
  } else {
    printf("error: did not find a\n");
  }
  flow_record_list_remove(&list, &a);
  rp = flow_record_list_find_record_by_key(&list, &k1);
  if (!rp) {
    printf("didn't find a\n");
  } else {
    printf("error: found a, but should not have\n");
  }
  flow_record_list_prepend(&list, &b);
  rp = flow_record_list_find_record_by_key(&list, &k2);
  if (rp) {
    printf("found b\n");
  } else {
    printf("error: did not find b\n");
  }
  flow_record_list_remove(&list, &b);
  rp = flow_record_list_find_record_by_key(&list, &k2);
  if (!rp) {
    printf("didn't find b\n");
  } else {
    printf("error: found b, but should not have\n");
  }

  flow_record_list_prepend(&list, &a);
  flow_record_list_prepend(&list, &b);
  rp = flow_record_list_find_record_by_key(&list, &k1);
  if (rp) {
    printf("found a\n");
  } else {
    printf("error: did not find a\n");
  }
  rp = flow_record_list_find_record_by_key(&list, &k2);
  if (rp) {
    printf("found b\n");
  } else {
    printf("error: did not find b\n");
  }
  flow_record_list_remove(&list, &a);
  rp = flow_record_list_find_record_by_key(&list, &k1);
  if (!rp) {
    printf("didn't find a\n");
  } else {
    printf("error: found a, but should not have\n");
  }
  flow_record_list_remove(&list, &b);
  rp = flow_record_list_find_record_by_key(&list, &k2);
  if (!rp) {
    printf("didn't find b\n");
  } else {
    printf("error: found a, but should not have\n");
  }

  flow_record_list_prepend(&list, &a);
  flow_record_list_prepend(&list, &c);
  rp = flow_record_list_find_record_by_key(&list, &k1);
  if (rp) {
    printf("found a\n");
  } else {
    printf("error: did not find a\n");
  }
  
}

void flow_record_chrono_list_append(struct flow_record *record) {
  extern struct flow_record *flow_record_chrono_first;
  extern struct flow_record *flow_record_chrono_last;

  if (flow_record_chrono_first == NULL) {
    // fprintf(info, "CHRONO flow_record_chrono_first == NULL, setting to %p ------------------\n", record);
    flow_record_chrono_first = record;
    flow_record_chrono_last = record;
  } else {
    // fprintf(info, "CHRONO last == %p, setting to %p\n", flow_record_chrono_last, record);
    // fprintf(info, "CHRONO last->time_next == %p, setting to %p\n", flow_record_chrono_last->time_next, record);
    flow_record_chrono_last->time_next = record;
    record->time_prev = flow_record_chrono_last;
    flow_record_chrono_last = record;
    // fprintf(info, "CHRONO last->time_next == %p\n", flow_record_chrono_last->time_next);
  }
}

inline unsigned int flow_record_is_in_chrono_list(const struct flow_record *record) {
  return record->time_next || record->time_prev;
}

void flow_record_chrono_list_remove(struct flow_record *record) {
  extern struct flow_record *flow_record_chrono_first;
  extern struct flow_record *flow_record_chrono_last;

  if (record == NULL) {
    return;   /* sanity check - don't ever go here */
  }

  if (record == flow_record_chrono_first) {
    flow_record_chrono_first = record->time_next;
  } 
  if (record == flow_record_chrono_last) {
    flow_record_chrono_last = record->time_prev;
  } 

  if (record->time_prev != NULL) {
    record->time_prev->time_next = record->time_next;
  }
  if (record->time_next != NULL) {
    record->time_next->time_prev = record->time_prev;
  }
  
}

struct flow_record *flow_record_chrono_list_get_first() {
  return flow_record_chrono_first;
}

unsigned int flow_record_is_past_active_expiration(const struct flow_record *record) {
  if (record->end.tv_sec > (record->start.tv_sec + active_max)) { 
    if ((record->twin == NULL) || (record->end.tv_sec > (record->twin->start.tv_sec + active_max))) {
      return 1;
    }
  }
  return 0;
}

struct flow_record *flow_key_get_record(const struct flow_key *key, 
					unsigned int create_new_records) {
  struct flow_record *record;
  unsigned int hash_key;

  /* find a record matching the flow key, if it exists */
  hash_key = flow_key_hash(key);
  record = flow_record_list_find_record_by_key(&flow_record_list_array[hash_key], key);
  if (record != NULL) {
    if (create_new_records && flow_record_is_in_chrono_list(record) && flow_record_is_past_active_expiration(record)) {
    /* 
     *  active-timeout exceeded for this flow_record; print and delete
     *  it, then set record = NULL to cause the creation of a new
     *  flow_record to be used in further packet processing
     */
      // fprintf(output, "deleting active-expired record\n");
      flow_record_print_and_delete(record);
      record = NULL;
    } else {
      return record;
    }
  }

  /* if we get here, then record == NULL  */
  
  if (create_new_records) {

    /* allocate and initialize a new flow record */    
    record = malloc(sizeof(struct flow_record));
    debug_printf("LIST record %p allocated\n", record);
    
    if (record == NULL) {
      fprintf(info, "warning: could not allocate memory for flow_record\n");
      flocap_stats_incr_malloc_fail();
      return NULL;
    }
    
    flow_record_init(record, key);
    
    /* enter record into flow_record_list */
    flow_record_list_prepend(&flow_record_list_array[hash_key], record);
        
    /*
     * if we are tracking bidirectional flows, and if record has a
     * twin, then set both twin pointers; otherwise, enter the
     * record into the chronological list
     */
    if (bidir) {
      record->twin = flow_key_get_twin(key);
      debug_printf("LIST record %p is twin of %p\n", record, record->twin);
    } 
    if (record->twin != NULL) {
      if (record->twin->twin != NULL) {
	fprintf(info, "warning: found twin that already has a twin; not setting twin pointer\n");
	debug_printf("\trecord:    (hash key %x)(addr: %p)\n", flow_key_hash(&record->key), record);
	debug_printf("\ttwin:      (hash key %x)(addr: %p)\n", flow_key_hash(&record->twin->key), &record->twin);
	debug_printf("\ttwin twin: (hash key %x)(addr: %p)\n", flow_key_hash(&record->twin->twin->key), &record->twin->key);
      } else {
	record->twin->twin = record;
      }
    } else {
      
      /* this flow has no twin, so add it to chronological list */
      flow_record_chrono_list_append(record);      
    }
  } 
  
  return record;
} 

void flow_record_delete(struct flow_record *r) {
  unsigned int i;

  //  hash_key = flow_key_hash(&r->key);
  if (flow_record_list_remove(&flow_record_list_array[flow_key_hash(&r->key)], r) != 0) {
    fprintf(info, "warning: error removing flow record %p from list\n", r);
    return;
  }

  flocap_stats_decr_records_in_table();

  /*
   * free the memory allocated inside of flow record
   */
  for (i=0; i<MAX_NUM_PKT_LEN; i++) {
    if (r->dns_name[i]) {
      free(r->dns_name[i]);
    }
  }
  if (r->idp) {
    free(r->idp);
  }
  tls_record_delete(&r->tls_info);

  if (r->exe_name) {
    free(r->exe_name);
  }
  /*
   * zeroize memory (this is defensive coding; pointers to deleted
   * records will result in crashes rather than silent errors)
   */
  memset(r, 0, sizeof(struct flow_record));
  free(r);

}

int flow_key_set_exe_name(const struct flow_key *key, const char *name) {
  struct flow_record *r;

  if (name == NULL) {
    return failure;   /* no point in looking for flow_record */
  }
  r = flow_key_get_record(key, DONT_CREATE_RECORDS);
  // flow_key_print(key);
  if (r) {
    if (r->exe_name == NULL) {
      r->exe_name = strdup(name);
      return ok;
    }
  }
  return failure;
}

void flow_record_update_byte_count(struct flow_record *f, const void *x, unsigned int len) {
  const unsigned char *data = x;
  int i;

  if (byte_distribution || report_entropy) {
    for (i=0; i<len; i++) {
      f->byte_count[data[i]]++;
    }
  }

  /*
   * implementation note: overflow might occur in the byte_count
   * array; if the integer type for that array is small, then we 
   * should check for overflow and rebalance the array as needed
   */

}

void flow_record_update_byte_dist_mean_var(struct flow_record *f, const void *x, unsigned int len) {
  const unsigned char *data = x;
  double delta;
  int i;

  if (byte_distribution || report_entropy) {
    for (i=0; i<len; i++) {
      f->num_bytes += 1;
      delta = ((double)data[i] - f->bd_mean);
      f->bd_mean += delta/((double)f->num_bytes);
      f->bd_variance += delta*((double)data[i] - f->bd_mean);
    }
  }
}

#include <math.h>
#include <float.h>   /* for FLT_EPSILON */

float flow_record_get_byte_count_entropy(const unsigned int byte_count[256], 
					 unsigned int num_bytes) {
  int i;
  float tmp, sum = 0.0;

  for (i=0; i<256; i++) {
    tmp = (float) byte_count[i] / (float) num_bytes;
    if (tmp > FLT_EPSILON) {
      sum -= tmp * logf(tmp);
    }
    // fprintf(output, "tmp: %f\tsum: %f\n", tmp, sum);
  }
  return sum / logf(2.0);
}

void mem_print(const void *mem, unsigned int len) {
  const unsigned char *x = mem;

  while (len-- > 0) {
    fprintf(output, "%02x", *x++);
  }
  fprintf(output, "\n");
}

void flow_key_print(const struct flow_key *key) {
  debug_printf("flow key:\n");
  debug_printf("\tsa: %s\n", inet_ntoa(key->sa));
  debug_printf("\tda: %s\n", inet_ntoa(key->da));
  debug_printf("\tsp: %u\n", key->sp);
  debug_printf("\tdp: %u\n", key->dp);
  debug_printf("\tpr: %u\n", key->prot);
  mem_print(key, sizeof(struct flow_key));
}

void flow_record_print(const struct flow_record *record) {
  unsigned int i, imax;
  char addr_string[INET6_ADDRSTRLEN];

  fprintf(output, "flow record:\n");
  if (ipv4_addr_needs_anonymization(&record->key.sa)) {
    fprintf(output, "\tsa: %s\n", addr_get_anon_hexstring(&record->key.sa));
  } else {
    inet_ntop(AF_INET, &record->key.sa, addr_string, INET6_ADDRSTRLEN);
    fprintf(output, "\tsa: %s\n", addr_string);
  }
  if (ipv4_addr_needs_anonymization(&record->key.da)) {
    fprintf(output, "\tda: %s\n", addr_get_anon_hexstring(&record->key.da));
  } else {
    fprintf(output, "\tda: %s\n", inet_ntoa(record->key.da));
  }
  fprintf(output, "\tsp: %u\n", record->key.sp);
  fprintf(output, "\tdp: %u\n", record->key.dp);
  fprintf(output, "\tpr: %u\n", record->key.prot);
  fprintf(output, "\tob: %u\n", record->ob);
  fprintf(output, "\top: %u\n", record->np);  /* not just packets with data */
  fprintf(output, "\tttl: %u\n", record->ttl);  
  fprintf(output, "\tpkt_len: [ ");
  imax = record->op > num_pkt_len ? num_pkt_len : record->op;
  if (imax != 0) {
    for (i = 1; i < imax; i++) {
      fprintf(output, "%u, ", record->pkt_len[i-1]);
    }
    fprintf(output, "%u ", record->pkt_len[i-1]);
  }
  fprintf(output, "]\n");
  if (byte_distribution) {
    if (record->ob != 0) {
      fprintf(output, "\tbd: [ ");
      for (i = 0; i < 255; i++) {
	fprintf(output, "%u, ", record->byte_count[i]);
      }
      fprintf(output, "%u ]\n", record->byte_count[i]);
    }
  }
  if (report_entropy) {
    if (record->ob != 0) {
      fprintf(output, "\tbe: %f\n", 
	      flow_record_get_byte_count_entropy(record->byte_count, record->ob));
    }
  }
}

void print_bytes_dir_time(unsigned short int pkt_len, char *dir, struct timeval ts, char *term) {
  if (pkt_len < 32768) {
    fprintf(output, "\t\t\t\t{ \"b\": %u, \"dir\": \"%s\", \"ipt\": %u }%s", 
	    pkt_len, dir, timeval_to_milliseconds(ts), term);
  } else {
    fprintf(output, "\t\t\t\t{ \"rep\": %u, \"dir\": \"%s\", \"ipt\": %u }%s", 
	    65536-pkt_len, dir, timeval_to_milliseconds(ts), term);    
  }
}

void print_bytes_dir_time_type(unsigned short int pkt_len, char *dir, struct timeval ts, struct tls_type_code type, char *term) {

  fprintf(output, "\t\t\t\t{ \"b\": %u, \"dir\": \"%s\", \"ipt\": %u, \"tp\": \"%u:%u\" }%s", 
	  pkt_len, dir, timeval_to_milliseconds(ts), type.content, type.handshake, term);

}

#define OUT "<"
#define IN  ">"

void len_time_print_interleaved(unsigned int op, const unsigned short *len, const struct timeval *time, const struct tls_type_code *type,
				unsigned int op2, const unsigned short *len2, const struct timeval *time2, const struct tls_type_code *type2) {
  unsigned int i, j, imax, jmax;
  struct timeval ts, ts_last, ts_start, tmp;
  unsigned int pkt_len;
  char *dir;
  struct tls_type_code typecode;

  fprintf(output, ",\n\t\t\t\"tls\": [\n");

  if (len2 == NULL) {
    
    ts_start = *time;

    imax = op > num_pkt_len ? num_pkt_len : op;
    if (imax == 0) { 
      ; /* no packets had data, so we print out nothing */
    } else {
      for (i = 0; i < imax-1; i++) {
	if (i > 0) {
	  timer_sub(&time[i], &time[i-1], &ts);
	} else {
	  timer_clear(&ts);
	}
	print_bytes_dir_time_type(len[i], OUT, ts, type[i], ",\n");
	// fprintf(output, "\t\t\t\t{ \"b\": %u, \"dir\": \">\", \"ipt\": %u },\n", 
	//    len[i], timeval_to_milliseconds(ts));
      }
      if (i == 0) {        /* this code could be simplified */ 	
	timer_clear(&ts);  
      } else {
	timer_sub(&time[i], &time[i-1], &ts);
      }
      print_bytes_dir_time_type(len[i], OUT, ts, type[i], "\n");
      // fprintf(output, "\t\t\t\t{ \"b\": %u, \"dir\": \">\", \"ipt\": %u }\n", 
      //    len[i], timeval_to_milliseconds(ts));
    }
    fprintf(output, "\t\t\t]"); 
  } else {

    if (timer_lt(time, time2)) {
      ts_start = *time;
    } else {
      ts_start = *time2;
    }

    imax = op > num_pkt_len ? num_pkt_len : op;
    jmax = op2 > num_pkt_len ? num_pkt_len : op2;
    i = j = 0;
    ts_last = ts_start;
    while ((i < imax) || (j < jmax)) {      

      if (i >= imax) {  /* record list is exhausted, so use twin */
	dir = OUT;
	ts = time2[j];
	pkt_len = len2[j];
	typecode = type2[j];
	j++;
      } else if (j >= jmax) {  /* twin list is exhausted, so use record */
	dir = IN;
	ts = time[i];
	pkt_len = len[i];
	typecode = type[i];
	i++;
      } else { /* neither list is exhausted, so use list with lowest time */     

	if (timer_lt(&time[i], &time2[j])) {
	  ts = time[i];
	  pkt_len = len[i];
	  typecode = type[i];
	  dir = IN;
	  if (i < imax) {
	    i++;
	  }
	} else {
	  ts = time2[j];
	  pkt_len = len2[j];
	  typecode = type2[j];
	  dir = OUT;
	  if (j < jmax) {
	    j++;
	  }
	}
      }
      // fprintf(output, "i: %d\tj: %d\timax: %d\t jmax: %d", i, j, imax, jmax);
      timer_sub(&ts, &ts_last, &tmp);
      //      fprintf(output, "\t\t\t\t{ \"b\": %u, \"dir\": \"%s\", \"ipt\": %u }", 
      //     pkt_len, dir, timeval_to_milliseconds(tmp));
      print_bytes_dir_time_type(pkt_len, dir, tmp, typecode, "");
      ts_last = ts;
      if ((i == imax) & (j == jmax)) { /* we are done */
      	fprintf(output, "\n"); 
      } else {
	fprintf(output, ",\n");
      }
    }
    fprintf(output, "\t\t\t]");
  }

}

void fprintf_raw_as_hex(FILE *f, const void *data, unsigned int len) {
  const unsigned char *x = data;
  const unsigned char *end = data + len;
  
  fprintf(f, "\"");   /* quotes needed for JSON */
  while (x < end) {
    fprintf(f, "%02x", *x++);
  }
  fprintf(f, "\"");

}



void flow_record_print_json(const struct flow_record *record) {
  unsigned int i, j, imax, jmax;
  struct timeval ts, ts_last, ts_start, ts_end, tmp;
  const struct flow_record *rec;
  unsigned int pkt_len;
  char *dir;

  if (records_in_file != 0) {
    fprintf(output, ",\n");
  }
 
  flocap_stats_incr_records_output();
  records_in_file++;

  if (record->twin != NULL) {
    if (timer_lt(&record->start, &record->twin->start)) {
      ts_start = record->start;
      rec = record;
    } else {
      ts_start = record->twin->start;
      rec = record->twin;
    }
    if (timer_lt(&record->end, &record->twin->end)) {
      ts_end = record->end;
    } else {
      ts_end = record->twin->end;
    }
  } else {
    ts_start = record->start;
    ts_end = record->end;
    rec = record;
  }

  fprintf(output, "\t\{\n\t\t\"flow\": {\n");

  /* print flow key */
  if (ipv4_addr_needs_anonymization(&rec->key.sa)) {
    fprintf(output, "\t\t\t\"sa\": \"%s\",\n", addr_get_anon_hexstring(&rec->key.sa));
  } else {
    fprintf(output, "\t\t\t\"sa\": \"%s\",\n", inet_ntoa(rec->key.sa));
  }
  if (ipv4_addr_needs_anonymization(&rec->key.da)) {
    fprintf(output, "\t\t\t\"da\": \"%s\",\n", addr_get_anon_hexstring(&rec->key.da));
  } else {
    fprintf(output, "\t\t\t\"da\": \"%s\",\n", inet_ntoa(rec->key.da));
  }
  fprintf(output, "\t\t\t\"pr\": %u,\n", rec->key.prot);
  if (1 || rec->key.prot == 6 || rec->key.prot == 17) {
    fprintf(output, "\t\t\t\"sp\": %u,\n", rec->key.sp);
    fprintf(output, "\t\t\t\"dp\": %u,\n", rec->key.dp);
  }

  /* 
   * if src or dst address matches a subnets associated with labels,
   * then print out those labels
   */
  if (config.num_subnets) {
    attr_flags flag;

    flag = radix_trie_lookup_addr(rt, rec->key.sa);
    attr_flags_json_print_labels(rt, flag, "sa_labels", output);
    flag = radix_trie_lookup_addr(rt, rec->key.da);
    attr_flags_json_print_labels(rt, flag, "da_labels", output);
  }

  /* print flow stats */
  fprintf(output, "\t\t\t\"ob\": %u,\n", rec->ob);
  fprintf(output, "\t\t\t\"op\": %u,\n", rec->np); /* not just packets with data */
  if (rec->twin != NULL) {
    fprintf(output, "\t\t\t\"ib\": %u,\n", rec->twin->ob);
    fprintf(output, "\t\t\t\"ip\": %u,\n", rec->twin->np);
  }
  fprintf(output, "\t\t\t\"ts\": %ld.%06ld,\n", ts_start.tv_sec, ts_start.tv_usec);
  fprintf(output, "\t\t\t\"te\": %ld.%06ld,\n", ts_end.tv_sec, ts_end.tv_usec);
  fprintf(output, "\t\t\t\"ottl\": %u,\n", rec->ttl);
  if (rec->twin != NULL) {
    fprintf(output, "\t\t\t\"ittl\": %u,\n", rec->twin->ttl);
  }

  if (rec->tcp_initial_window_size) {
    fprintf(output, "\t\t\t\"otcp_win\": %u,\n", rec->tcp_initial_window_size);
  }
  if (rec->twin != NULL) {
    if (rec->twin->tcp_initial_window_size) {
      fprintf(output, "\t\t\t\"itcp_win\": %u,\n", rec->twin->tcp_initial_window_size);
    }
  }

  if (rec->tcp_syn_size) {
    fprintf(output, "\t\t\t\"otcp_syn\": %u,\n", rec->tcp_syn_size);
  }
  if (rec->twin != NULL) {
    if (rec->twin->tcp_syn_size) {
      fprintf(output, "\t\t\t\"itcp_syn\": %u,\n", rec->twin->tcp_syn_size);
    }
  }

  if (rec->tcp_option_nop) {
    fprintf(output, "\t\t\t\"otcp_nop\": %u,\n", rec->tcp_option_nop);
  }
  if (rec->twin != NULL) {
    if (rec->twin->tcp_option_nop) {
      fprintf(output, "\t\t\t\"itcp_nop\": %u,\n", rec->twin->tcp_option_nop);
    }
  }

  if (rec->tcp_option_mss) {
    fprintf(output, "\t\t\t\"otcp_mss\": %u,\n", rec->tcp_option_mss);
  }
  if (rec->twin != NULL) {
    if (rec->twin->tcp_option_mss) {
      fprintf(output, "\t\t\t\"itcp_mss\": %u,\n", rec->twin->tcp_option_mss);
    }
  }

  if (rec->tcp_option_wscale) {
    fprintf(output, "\t\t\t\"otcp_wscale\": %u,\n", rec->tcp_option_wscale);
  }
  if (rec->twin != NULL) {
    if (rec->twin->tcp_option_wscale) {
      fprintf(output, "\t\t\t\"itcp_wscale\": %u,\n", rec->twin->tcp_option_wscale);
    }
  }

  if (rec->tcp_option_sack) {
    fprintf(output, "\t\t\t\"otcp_sack\": %u,\n", rec->tcp_option_sack);
  }
  if (rec->twin != NULL) {
    if (rec->twin->tcp_option_sack) {
      fprintf(output, "\t\t\t\"itcp_sack\": %u,\n", rec->twin->tcp_option_sack);
    }
  }

  if (rec->tcp_option_tstamp) {
    fprintf(output, "\t\t\t\"otcp_tstamp\": %u,\n", rec->tcp_option_tstamp);
  }
  if (rec->twin != NULL) {
    if (rec->twin->tcp_option_tstamp) {
      fprintf(output, "\t\t\t\"itcp_tstamp\": %u,\n", rec->twin->tcp_option_tstamp);
    }
  }

#if 0

  len_time_print_interleaved(rec->op, rec->pkt_len, rec->pkt_time,
			     rec->twin->op, rec->twin->pkt_len, rec->twin->pkt_time);
#else
  /* print length and time arrays */
  fprintf(output, "\t\t\t\"non_norm_stats\": [\n");

  if (rec->twin == NULL) {
    
    imax = rec->op > num_pkt_len ? num_pkt_len : rec->op;
    if (imax == 0) { 
      ; /* no packets had data, so we print out nothing */
    } else {
      for (i = 0; i < imax-1; i++) {
	if (i > 0) {
	  timer_sub(&rec->pkt_time[i], &rec->pkt_time[i-1], &ts);
	} else {
	  timer_clear(&ts);
	}
	print_bytes_dir_time(rec->pkt_len[i], OUT, ts, ",\n");
	// fprintf(output, "\t\t\t\t{ \"b\": %u, \"dir\": \">\", \"ipt\": %u },\n", 
	//    record->pkt_len[i], timeval_to_milliseconds(ts));
      }
      if (i == 0) {        /* this code could be simplified */ 	
	timer_clear(&ts);  
      } else {
	timer_sub(&rec->pkt_time[i], &rec->pkt_time[i-1], &ts);
      }
      print_bytes_dir_time(rec->pkt_len[i], OUT, ts, "\n");
      // fprintf(output, "\t\t\t\t{ \"b\": %u, \"dir\": \">\", \"ipt\": %u }\n", 
      //    record->pkt_len[i], timeval_to_milliseconds(ts));
    }
    fprintf(output, "\t\t\t]"); 
  } else {

    imax = rec->op > num_pkt_len ? num_pkt_len : rec->op;
    jmax = rec->twin->op > num_pkt_len ? num_pkt_len : rec->twin->op;
    i = j = 0;
    ts_last = ts_start;
    while ((i < imax) || (j < jmax)) {      

      if (i >= imax) {  /* record list is exhausted, so use twin */
	dir = OUT;
	ts = rec->twin->pkt_time[j];
	pkt_len = rec->twin->pkt_len[j];
	j++;
      } else if (j >= jmax) {  /* twin list is exhausted, so use record */
	dir = IN;
	ts = rec->pkt_time[i];
	pkt_len = rec->pkt_len[i];
	i++;
      } else { /* neither list is exhausted, so use list with lowest time */     

	if (timer_lt(&rec->pkt_time[i], &rec->twin->pkt_time[j])) {
	  ts = rec->pkt_time[i];
	  pkt_len = rec->pkt_len[i];
	  dir = IN;
	  if (i < imax) {
	    i++;
	  }
	} else {
	  ts = rec->twin->pkt_time[j];
	  pkt_len = rec->twin->pkt_len[j];
	  dir = OUT;
	  if (j < jmax) {
	    j++;
	  }
	}
      }
      // fprintf(output, "i: %d\tj: %d\timax: %d\t jmax: %d", i, j, imax, jmax);
      timer_sub(&ts, &ts_last, &tmp);
      //      fprintf(output, "\t\t\t\t{ \"b\": %u, \"dir\": \"%s\", \"ipt\": %u }", 
      //     pkt_len, dir, timeval_to_milliseconds(tmp));
      print_bytes_dir_time(pkt_len, dir, tmp, "");
      ts_last = ts;
      if ((i == imax) & (j == jmax)) { /* we are done */
      	fprintf(output, "\n"); 
      } else {
	fprintf(output, ",\n");
      }
    }
    fprintf(output, "\t\t\t]");
  }
#endif /* 0 */

  if (byte_distribution || report_entropy) {
    const unsigned int *array;
    unsigned int tmp[256];
    unsigned int num_bytes;
    double mean = 0.0, variance = 0.0;

    /* 
     * sum up the byte_count array for outbound and inbound flows, if
     * this flow is bidirectional
     */
    if (rec->twin == NULL) {
      array = rec->byte_count;
      num_bytes = rec->ob;

      if (rec->num_bytes != 0) {
	mean = rec->bd_mean;
	variance = rec->bd_variance/(rec->num_bytes - 1);
	variance = sqrt(variance);
	if (rec->num_bytes == 1) {
	  variance = 0.0;
	}
      }
    } else {
      for (i=0; i<256; i++) {
	tmp[i] = rec->byte_count[i] + rec->twin->byte_count[i];
      }
      array = tmp;
      num_bytes = rec->ob + rec->twin->ob;

      if (rec->num_bytes + rec->twin->num_bytes != 0) {
	mean = ((double)rec->num_bytes)/((double)(rec->num_bytes+rec->twin->num_bytes))*rec->bd_mean +
	  ((double)rec->twin->num_bytes)/((double)(rec->num_bytes+rec->twin->num_bytes))*rec->twin->bd_mean;
	variance = ((double)rec->num_bytes)/((double)(rec->num_bytes+rec->twin->num_bytes))*rec->bd_variance +
	  ((double)rec->twin->num_bytes)/((double)(rec->num_bytes+rec->twin->num_bytes))*rec->twin->bd_variance;
	variance = variance/((double)(rec->num_bytes + rec->twin->num_bytes - 1));
	variance = sqrt(variance);
	if (rec->num_bytes + rec->twin->num_bytes == 1) {
	  variance = 0.0;
	}
      }
    }
    
    if (byte_distribution) {
      fprintf(output, ",\n\t\t\t\"bd\": [ ");
      for (i = 0; i < 255; i++) {
	if ((i % 16) == 0) {
	  fprintf(output, "\n\t\t\t        ");	    
	}
	fprintf(output, "%3u, ", array[i]);
      }
      fprintf(output, "%3u\n\t\t\t]", array[i]);

      // output the mean
      if (num_bytes != 0) {
	fprintf(output, ",\n\t\t\t\"bd_mean\": %f", mean);
	fprintf(output, ",\n\t\t\t\"bd_std\": %f", variance);
      }

    }

    if (report_entropy) {
      if (num_bytes != 0) {
	double entropy = flow_record_get_byte_count_entropy(array, num_bytes);
	
	fprintf(output, ",\n\t\t\t\"be\": %f", entropy);
	fprintf(output, ",\n\t\t\t\"tbe\": %f", entropy * num_bytes);
      }
    }
  }

  // inline classification of flows
  if (include_classifier) {
    float score = 0.0;
    
    if (rec->twin) {
      score = classify(rec->pkt_len, rec->pkt_time, rec->twin->pkt_len, rec->twin->pkt_time,
		       rec->start, rec->twin->start,
		       NUM_PKT_LEN, rec->key.sp, rec->key.dp, rec->np, rec->twin->np, rec->op, rec->twin->op,
		       rec->ob, rec->twin->ob, byte_distribution,
		       rec->byte_count, rec->twin->byte_count);
    } else {
      score = classify(rec->pkt_len, rec->pkt_time, NULL, NULL,	rec->start, rec->start,
		       NUM_PKT_LEN, rec->key.sp, rec->key.dp, rec->np, 0, rec->op, 0,
		       rec->ob, 0, byte_distribution,
		       rec->byte_count, NULL);
    }

    fprintf(output, ",\n\t\t\t\"p_malware\": \"%f\"", score);
  }

  if (report_wht) { 
    if (rec->twin) {
      wht_printf_scaled_bidir(&rec->wht, rec->ob, &rec->twin->wht, rec->twin->ob, output);
    } else {
      wht_printf_scaled(&rec->wht, output, rec->ob);
    }
  }

  if (report_hd) {
    /*
     * note: this should be bidirectional, but it is not!  This will
     * be changed sometime soon, but for now, this will give some
     * experience with this type of data
     */
    header_description_printf(&rec->hd, output, report_hd);
  }

  if (include_os) { 

    if (rec->twin) {
      os_printf(output, rec->ttl, rec->tcp_initial_window_size, rec->twin->ttl, rec->twin->tcp_initial_window_size);
    } else {
      os_printf(output, rec->ttl, rec->tcp_initial_window_size, 0, 0);
    }

  }

  if (include_tls) { 

    if (rec->tls_info.tls_v) {
      fprintf(output, ",\n\t\t\t\"tls_ov\": %u", rec->tls_info.tls_v);
      //      fprintf(output, ",\n\t\t\t\"tls_ov\": %s", tls_version_get_string(record->tls_info.tls_v));
    }
    if (rec->twin && rec->twin->tls_info.tls_v) {
      fprintf(output, ",\n\t\t\t\"tls_iv\": %u", rec->twin->tls_info.tls_v);
      //      fprintf(output, ",\n\t\t\t\"tls_iv\": %s", tls_version_get_string(record->twin->tls_info.tls_v));
    }

    if (rec->tls_info.tls_client_key_length) {
      fprintf(output, ",\n\t\t\t\"tls_client_key_length\": %u", rec->tls_info.tls_client_key_length);
    }
    if (rec->twin && rec->twin->tls_info.tls_client_key_length) {
      fprintf(output, ",\n\t\t\t\"tls_client_key_length\": %u", rec->twin->tls_info.tls_client_key_length);
    }

    /*
     * print out TLS random, using the ciphersuite count as a way to
     * determine whether or not we have seen a clientHello or a
     * serverHello
     */
    if (rec->tls_info.num_ciphersuites) {
      fprintf(output, ",\n\t\t\t\"tls_orandom\": ");
      fprintf_raw_as_hex(output, rec->tls_info.tls_random, 32);
    }
    if (rec->twin && rec->twin->tls_info.num_ciphersuites) {
      fprintf(output, ",\n\t\t\t\"tls_irandom\": ");
      fprintf_raw_as_hex(output, rec->twin->tls_info.tls_random, 32);
    }

    if (rec->tls_info.tls_sid_len) {
      fprintf(output, ",\n\t\t\t\"tls_osid\": ");
      fprintf_raw_as_hex(output, rec->tls_info.tls_sid, rec->tls_info.tls_sid_len);
    }

    if (rec->twin && rec->twin->tls_info.tls_sid_len) {
      fprintf(output, ",\n\t\t\t\"tls_isid\": ");
      fprintf_raw_as_hex(output, rec->twin->tls_info.tls_sid, rec->twin->tls_info.tls_sid_len);
    }

    if (rec->tls_info.num_ciphersuites) {
      if (rec->tls_info.num_ciphersuites == 1) {
	fprintf(output, ",\n\t\t\t\"scs\": \"%04x\"", rec->tls_info.ciphersuites[0]);
      } else {
	fprintf(output, ",\n\t\t\t\"cs\": [ ");
	for (i = 0; i < rec->tls_info.num_ciphersuites-1; i++) {
	  if ((i % 8) == 0) {
	    fprintf(output, "\n\t\t\t        ");	    
	  }
	  fprintf(output, "\"%04x\", ", rec->tls_info.ciphersuites[i]);
	}
	fprintf(output, "\"%04x\"\n\t\t\t]", rec->tls_info.ciphersuites[i]);
      }
    }  

    if (rec->twin && rec->twin->tls_info.num_ciphersuites) {
      if (rec->twin->tls_info.num_ciphersuites == 1) {
	fprintf(output, ",\n\t\t\t\"scs\": \"%04x\"", rec->tls_info.ciphersuites[0]);
      } else {
	fprintf(output, ",\n\t\t\t\"cs\": [ ");
	for (i = 0; i < rec->twin->tls_info.num_ciphersuites-1; i++) {
	  if ((i % 8) == 0) {
	    fprintf(output, "\n\t\t\t        ");	    
	  }
	  fprintf(output, "\"%04x\", ", rec->twin->tls_info.ciphersuites[i]);
	}
	fprintf(output, "\"%04x\"\n\t\t\t]", rec->twin->tls_info.ciphersuites[i]);
      }
    }    
  
    if (rec->tls_info.num_tls_extensions) {
      fprintf(output, ",\n\t\t\t\"tls_ext\": [ ");
      for (i = 0; i < rec->tls_info.num_tls_extensions-1; i++) {
	fprintf(output, "\n\t\t\t\t{ \"type\": \"%04x\", ", rec->tls_info.tls_extensions[i].type);
	fprintf(output, "\"length\": %i, \"data\": ", rec->tls_info.tls_extensions[i].length);
	fprintf_raw_as_hex(output, rec->tls_info.tls_extensions[i].data, rec->tls_info.tls_extensions[i].length);
	fprintf(output, "},");
      }
      fprintf(output, "\n\t\t\t\t{ \"type\": \"%04x\", ", rec->tls_info.tls_extensions[i].type);
      fprintf(output, "\"length\": %i, \"data\": ", rec->tls_info.tls_extensions[i].length);
      fprintf_raw_as_hex(output, rec->tls_info.tls_extensions[i].data, rec->tls_info.tls_extensions[i].length);
      fprintf(output, "}\n\t\t\t]");
    }  
    if (rec->twin && rec->twin->tls_info.num_tls_extensions) {
      fprintf(output, ",\n\t\t\t\"tls_ext\": [ ");
      for (i = 0; i < rec->twin->tls_info.num_tls_extensions-1; i++) {
	fprintf(output, "\n\t\t\t\t{ \"type\": \"%04x\", ", rec->twin->tls_info.tls_extensions[i].type);
	fprintf(output, "\"length\": %i, \"data\": ", rec->twin->tls_info.tls_extensions[i].length);
	fprintf_raw_as_hex(output, rec->twin->tls_info.tls_extensions[i].data, rec->twin->tls_info.tls_extensions[i].length);
	fprintf(output, "},");
      }
      fprintf(output, "\n\t\t\t\t{ \"type\": \"%04x\", ", rec->twin->tls_info.tls_extensions[i].type);
      fprintf(output, "\"length\": %i, \"data\": ", rec->twin->tls_info.tls_extensions[i].length);
      fprintf_raw_as_hex(output, rec->twin->tls_info.tls_extensions[i].data, rec->twin->tls_info.tls_extensions[i].length);
      fprintf(output, "}\n\t\t\t]");
    }

  
    /* print out TLS application data lengths and times, if any */
    if (rec->tls_info.tls_op) {
      if (rec->twin) {
	len_time_print_interleaved(rec->tls_info.tls_op, rec->tls_info.tls_len, rec->tls_info.tls_time, rec->tls_info.tls_type,
				   rec->twin->tls_info.tls_op, rec->twin->tls_info.tls_len, rec->twin->tls_info.tls_time, rec->twin->tls_info.tls_type);
      } else {
	/*
	 * unidirectional TLS does not typically happen, but if it
	 * does, we need to pass in zero/NULLs, since there is no twin
	 */
	len_time_print_interleaved(rec->tls_info.tls_op, rec->tls_info.tls_len, rec->tls_info.tls_time, rec->tls_info.tls_type, 0, NULL, NULL, NULL);
      }
    }
  }

  if (report_idp) {
    if (rec->idp != NULL) {
      fprintf(output, ",\n\t\t\t\"oidp\": ");
      fprintf_raw_as_hex(output, rec->idp, rec->idp_len);
      fprintf(output, ",\n\t\t\t\"oidp_len\": %u", rec->idp_len);
    }
    if (rec->twin && (rec->twin->idp != NULL)) {
      fprintf(output, ",\n\t\t\t\"iidp\": ");
      fprintf_raw_as_hex(output, rec->twin->idp, rec->twin->idp_len);
      fprintf(output, ",\n\t\t\t\"iidp_len\": %u", rec->twin->idp_len);
    }
  }

  if (report_dns && (rec->key.sp == 53 || rec->key.dp == 53)) {
    unsigned int count;

    fprintf(output, ",\n\t\t\t\"dns\": [");
    
    count = rec->op > MAX_NUM_PKT_LEN ? MAX_NUM_PKT_LEN : rec->op;

    if (rec->twin) {
      char *q, *r;

      count = rec->twin->op > count ? rec->twin->op : count;
      for (i=0; i<count; i++) {
	if (i) {
	  fprintf(output, ",");
	}
	if (rec->dns_name[i]) {
	  q = rec->dns_name[i];
	  convert_string_to_printable(q, rec->pkt_len[i] - 13);
	} else {
	  q = "";
	}
	if (rec->twin->dns_name[i]) {
	  r = rec->twin->dns_name[i];
	  convert_string_to_printable(r, rec->twin->pkt_len[i] - 13);
	} else {
	  r = "";
	}
	fprintf(output, "\n\t\t\t\t{ \"qn\": \"%s\", \"rn\": \"%s\" }", q, r);
      }
      
    } else { /* unidirectional flow, with no twin */

      for (i=0; i<count; i++) {
	if (i) {
	  fprintf(output, ",");
	}
	if (rec->dns_name[i]) {
	  convert_string_to_printable(rec->dns_name[i], rec->pkt_len[i] - 13);
	  fprintf(output, "\n\t\t\t\t{ \"qn\": \"%s\" }", rec->dns_name[i]);
	}
      }
    }

    fprintf(output, "\n\t\t\t]");
  }
  
  { 
    unsigned int retrans, invalid;
    
    retrans = rec->retrans;
    invalid = rec->invalid;
    if (rec->twin) {
      retrans += rec->twin->retrans;
      invalid += rec->twin->invalid;
    }
    if (retrans) {
      fprintf(output, ",\n\t\t\t\"rtn\": %u", retrans);
    }
    if (invalid) {
      fprintf(output, ",\n\t\t\t\"inv\": %u", invalid);
    }

  }

  if (rec->exe_name) {
    fprintf(output, ",\n\t\t\t\"exe\": \"%s\"", rec->exe_name);
  }

  if (rec->exp_type) {
    fprintf(output, ",\n\t\t\t\"x\": \"%c\"", rec->exp_type);
  }

  fprintf(output, "\n\t\t}\n\t}");

}


void flow_record_print_time_to_expiration(const struct flow_record *r, 
					  const struct timeval *inactive_cutoff) {
  struct timeval tte_active, tte_inactive, active_expiration;

  timer_sub(&r->end, inactive_cutoff, &tte_inactive);
  timer_sub(inactive_cutoff, &active_timeout, &active_expiration);
  timer_sub(&r->start, &active_expiration, &tte_active); 
  fprintf(info, "seconds to expiration - active: %f inactive %f\n", 
	  ((float) timeval_to_milliseconds(tte_active)) / 1000.0,
	  ((float) timeval_to_milliseconds(tte_inactive) / 1000.0));

}

/*
 * a unidirectional flow_record is inactive-expired when it end time is after
 * the expiration time
 *
 * a bidirectional flow_record (i.e. one with twin != NULL) is expired
 * when both the record end time and the twin end time are after the
 * expiration time
 *
 */
unsigned int flow_record_is_inactive(struct flow_record *record,
				     const struct timeval *expiration) {

  if (timer_lt(&record->end, expiration)) {
    if (record->twin) {
      if (timer_lt(&record->twin->end, expiration)) {
	//  fprintf(info, "bidir flow past inactive cutoff\n");
	record->exp_type = expiration_type_inactive;
	return 1;
      }
    } else {
      // fprintf(info, "undir flow past inactive cutoff\n");
      record->exp_type = expiration_type_inactive;
      return 1;
    }
  }
  // fprintf(info, "no inactive cutoff\n");
  return 0;
}

unsigned int flow_record_is_expired(struct flow_record *record,
				    const struct timeval *inactive_cutoff) {
  struct timeval active_expiration;

  /*
   * check for active timeout
   */ 
  timer_sub(inactive_cutoff, &active_timeout, &active_expiration);

  if (timer_lt(&record->start, &active_expiration)) {
    if (record->twin) {
      if (timer_lt(&record->twin->start, &active_expiration)) {
	record->exp_type = expiration_type_active;
	// fprintf(info, "bidir flow past active cutoff\n");
	return 1;
      }
    } else {
      record->exp_type = expiration_type_active;
      // fprintf(info, "unidir flow past active cutoff\n");
      return 1;
    }
  }
  // fprintf(info, "no active cutoff\t");
  return flow_record_is_inactive(record, inactive_cutoff);
}

void flow_record_print_and_delete(struct flow_record *record) {
  
  flow_record_print_json(record);
  
  /* delete twin, if there is one */
  if (record->twin != NULL) {
    debug_printf("LIST deleting twin\n");
    flow_record_delete(record->twin);
    //      fprintf(info, "DELETING TWIN: %p\n", record->twin);
  }
  
  /* remove record from chrono list, then delete from flow_record_list_array */
  flow_record_chrono_list_remove(record);
  flow_record_delete(record);    
  
}

void flow_record_list_print_json(const struct timeval *inactive_cutoff) {
  struct flow_record *record;
  // unsigned int num_printed = 0;

  record = flow_record_chrono_list_get_first();
  while (record != NULL) {
    /*
     * avoid printing flows that might still be active, if a non-NULL
     * expiration time was passed into this function
     */
    if (inactive_cutoff && !flow_record_is_expired(record, inactive_cutoff)) {
      // fprintf(info, "BREAK: "); 
      // flow_record_print_time_to_expiration(record, inactive_cutoff);
      // flocap_stats_output(info);
      break;
    } 
    
    flow_record_print_and_delete(record);

    /* advance to next record on chrono list */  
    record = flow_record_chrono_list_get_first();
  }
  // fprintf(output, "] }\n");
  // fprintf(info, "printed %u records\n", num_printed);

  fflush(output);
}

void flow_record_list_print(const struct timeval *expiration) {
  struct flow_record *record;
  unsigned int count = 0;

  record = flow_record_chrono_first;
  while (record != NULL) {
    /*
     * avoid printing flows that might still be active, if a non-NULL
     * expiration time was passed into this function
     */
    if (expiration && timer_gt(&record->end, expiration)) {
      break;
    }
    flow_record_print(record);
    count++;
    record = record->time_next;
  }
  fprintf(output, "printed %u flow records\n", count);
}

struct flow_record *flow_key_get_twin(const struct flow_key *key) {
  if (config.flow_key_match_method == exact) {
    struct flow_key twin;

    /*
     * we use find_record_by_key() instead of find_twin_by_key(),
     * because we are using a flow_key_hash() that depends on the
     * entire flow key, and that hash won't work with
     * find_twin_by_key() function because it does not map near twins
     * to the same flow_record_list
     */
    twin.sa.s_addr = key->da.s_addr;
    twin.da.s_addr = key->sa.s_addr;
    twin.sp = key->dp;
    twin.dp = key->sp;
    twin.prot = key->prot;

    return flow_record_list_find_record_by_key(&flow_record_list_array[flow_key_hash(&twin)], &twin);
  
  } else {

  return flow_record_list_find_twin_by_key(&flow_record_list_array[flow_key_hash(key)], key);
  }
}


/*
 * The function flow_record_list_find_twins() is DEPRECATED, since
 * flow_key_get_record() now finds a twin for a newly created flow, if
 * that flow exists, and sets the twin pointer at that point.
 */
void flow_record_list_find_twins(const struct timeval *expiration) {
  struct flow_record *record, *twin, *parent;
  struct flow_key key;

  parent = record = flow_record_chrono_first;
  while (record != NULL) {
    /*
     * process only older, inactive flows, if a non-NULL expiration
     * time was passed into this function
     */
    if (expiration && timer_gt(&record->end, expiration)) {
      // fprintf(output, "record:     %u\n", timeval_to_milliseconds(record->end));
      // fprintf(output, "expiration: %u\n", timeval_to_milliseconds(*expiration));
      // fprintf(output, "find_twins reached end of expired flows\n");
      break;
    }

    key.sa = record->key.da;
    key.da = record->key.sa;
    key.sp = record->key.dp;
    key.dp = record->key.sp;
    key.prot = record->key.prot;
    
    twin = flow_key_get_record(&key, DONT_CREATE_RECORDS);
    if (twin != NULL) {
      /* sanity check */
      if (twin == record) {
	debug_printf("error: flow should not be its own twin\n");
      } else {
	// fprintf(output, "found twins\n");
	// flow_key_print(&key);
	// flow_key_print(&record->key);
	twin->twin = record;
	record->twin = twin;
	parent->time_next = record->time_next; /* remove record from chrono list */ 
      } 
    }
    if (parent != record) {
      parent = parent->time_next;
    }
    record = record->time_next;
  }
}


/* END flow monitoring */




/* BEGIN session termination */

/*
 * TCP session termination model
 * 
 * Sessions to be terminated are entered into the session table; each
 * entry holds the the network five-tuple of that session, in the form
 * of a flow_key structure.  To terminate an ongoing active session,
 * the flow record is looked up, and a pointer to the session termination
 * function is made.
 * 
 * It would be better to have a traffic filter associated with a set
 * of actions.  Perhaps there could be a separate BPF and pcap_handle
 * associated with each action.  Then there could be a
 * tcp_session_termination function associated with a BPF filter
 * expression.   
 *
 */

#if 0

#include <sys/socket.h>
#include <netinet/in.h>

int raw_socket;

int session_termination_init() {
  raw_socket = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (raw_socket == -1) {
    return failure;
  }
  return ok;
}


#define MAX_PKT_LEN 2048

int tcp_construct_reset(const struct ip_hdr *ip, const struct tcp_hdr *tcp, void *pkt) {
  struct ip_hdr *reset = pkt;
  struct tcp_hdr *reset_tcp;
  
  /* set IP header */
  reset->ip_vhl = ip->ip_vhl;
  reset->ip_tos = ip->ip_tos;
  reset->ip_len = 40;             /* is this always right? */
  reset->ip_id = ip->ip_id;
  reset->ip_off = ip->ip_off;
  reset->ip_ttl = 255;            /* this should be more carefully chosen */
  reset->ip_prot = ip->ip_prot;   /* 6, for TCP */
  reset->ip_cksum = 0;            /* to be set later */
  reset->ip_src.s_addr = ip->ip_dst.s_addr;
  reset->ip_dst.s_addr = ip->ip_src.s_addr;

  /* set TCP header */
  reset_tcp = pkt + sizeof(struct ip_hdr);
  reset_tcp->src_port = tcp->dst_port;
  reset_tcp->dst_port = tcp->src_port;
  reset_tcp->tcp_seq = 0xcafebabe;   /* should be set randomly */
  reset_tcp->tcp_ack = tcp->tcp_seq;
  reset_tcp->tcp_offrsv = 0;
  reset_tcp->tcp_flags = TCP_RST;
  reset_tcp->tcp_win = 0;
  reset_tcp->tcp_csm = 0;          /* to be set later */
  reset_tcp->tcp_urp = 0;
  
  return ok;
}

int packet_terminate_session(unsigned char *ignore, const struct pcap_pkthdr *header, const unsigned char *packet) {
  const struct ip_hdr *ip;              
  unsigned int transport_len;
  int size_ip;
  const void *transport_start;
  unsigned char pkt[MAX_PKT_LEN];
  struct sockaddr_in target;

  if (output_level > none) {
    fprintf(output, "terminating session\n");
  }
  
  /* define/compute ip header offset */
  ip = (struct ip_hdr*)(packet + ETHERNET_HDR_LEN);
  size_ip = ip_hdr_length(ip);
  if (size_ip < 20) {
    if (output_level > none) fprintf(output, "   * Invalid IP header length: %u bytes\n", size_ip);
    return failure;
  }

  /* print source and destination IP addresses */
  if (output_level > none) {
    fprintf(output, "       from: %s\n", inet_ntoa(ip->ip_src));
    fprintf(output, "         to: %s\n", inet_ntoa(ip->ip_dst));
  }
    
  /* determine transport protocol and handle appropriately */
  transport_len =  ntohs(ip->ip_len) - size_ip;
  transport_start = packet + ETHERNET_HDR_LEN + size_ip;
  switch(ip->ip_prot) {
  case IPPROTO_TCP:
    tcp_construct_reset(ip, transport_start, pkt);
    break;
  case IPPROTO_UDP:
    break;
  case IPPROTO_ICMP:
    break;    
  default:
    break;
  }

  /* send packet */
  target.sin_family = AF_INET;
  target.sin_port = 0;
  target.sin_addr.s_addr = ip->ip_src.s_addr;

  if (sendto(raw_socket, pkt, 40, 0, (struct sockaddr *)&target, sizeof(target)) != 40) {
    return failure;
  }

  return ok;
}


#endif /* 0/1 */

/* END session termination */



// #define RSYNC_CMD    "rsync", "rsync", "-avz", "-e", 
// #define RSYNC_CMD_RM "rsync", "rsync", "-avz", "-e", "scp -v -i upload-key -l data", "--remove-source-files"
// #define SCP_CMD      "scp", "scp", "-C", "-i", "/etc/flocap/upload-key"
#define SCP_CMD      "scp", "scp", "-C", "-i", config.upload_key

int upload_file(const char *filename, const char *servername, const char *key, unsigned int retain) {
  pid_t pid;
  static pid_t previous_pid = 0;
  int retval = -2;

  if (filename == NULL || servername == NULL || key == NULL) {
    fprintf(info, "error: could not upload file (output file, upload server, or keyfile not set\n");
    return failure;
  }

  pid = fork();
  if (pid == -1) {
    perror("error: could not fork");
    return -1;
  } else if (pid == 0) {
    
    /* we are the child process; exec command */
    if (retain) {
      retval = execlp(SCP_CMD, filename, servername, NULL);
    } else {
      retval = execlp(SCP_CMD, filename, servername, NULL);
    }
    if (retval == -1) {
      fprintf(info,
	      "error: could not exec command (rsync -avz [--remove-source-files] %s %s)",
	      filename, servername);
      exit(EXIT_FAILURE);
    }

  } else {
    
    /* we are the parent; check for previous zombie processes, then report success */    
    if (previous_pid && (waitpid(previous_pid, NULL, WNOHANG) == -1)) {
      perror("waitpid");
      exit(EXIT_FAILURE);
    }
    previous_pid = pid;

  }

  return 0;
}



#include <ctype.h>
/* 
 * convert_string_to_printable(s, len) convers the character string s
 * into a JSON-safe, NULL-terminated printable string.
 * Non-alphanumeric characters are converted to "." (a period).  This
 * function is useful only to ensure that strings that one expects to
 * be printable, such as DNS names, don't cause encoding errors when
 * they are actually not non-printable, non-JSON-safe strings.
 */ 

void convert_string_to_printable(char *s, unsigned int len) {
  unsigned int i;

  for (i=0; i<len; i++) {
    if (s[i] == 0) {
      break;
    } else if (!isalnum(s[i])) {
      s[i] = '.';
    }
  }
  s[len-1] = 0;  /* NULL termination */
}
