#include <stdio.h>  //fprintf
#include <stdlib.h> //free
#include <zlib.h>
#include <inttypes.h>
#include <math.h> // pow()
#include <stdint.h>

#include "kstring.h"
#include "kseq.h"
#include "khash.h"
#include "kvec.h"
#include "dna.h"

#define VERSION "0.0.3"

#define MIN_ASSEMBLY_K 15

KSEQ_INIT(gzFile, gzread)

typedef struct {
  uint32_t ref_kmer_id;
  double   pvalue;
  int      nb_merged_kmers;
  char     *seq;
} assembly_t;

void assembly_destroy(assembly_t *a) {
  if(a->seq)
    free(a->seq);
  free(a);
}

int cmp_assembly(const void * a, const void * b) {
  const assembly_t *ass_a = *(const assembly_t **)a;
  const assembly_t *ass_b = *(const assembly_t **)b;
  if(ass_a->ref_kmer_id == ass_b->ref_kmer_id) {
    return 0;
  } else {
    return (ass_a->ref_kmer_id - ass_b->ref_kmer_id);
  }
}

// Init k-mers hash
KHASH_MAP_INIT_INT64(kmers, uint32_t)

typedef khash_t(kmers) kmers_hash_t;
typedef kvec_t(assembly_t*) assemblies_array_t;

int main(int argc, char *argv[])
{
  char *counts_file;
  int k_length = 31;

  int c;
  while ((c = getopt(argc, argv, "k:")) >= 0) {
    switch (c) {
      case 'k': k_length = atoi(optarg); break;
    }
  }

  if (optind == argc) {
		fprintf(stderr, "\n");
		fprintf(stderr, "Usage:   mergeTags [options] <counts.tsv>\n\n");
		fprintf(stderr, "Options: -k INT    length of k-mers (max_value: 32) [%d]\n", k_length);
		fprintf(stderr, "\n");
		return 1;
	}

  counts_file = argv[optind++];

  fprintf(stderr, "Loading k-mers into memory\n");

  int nb_kmers = 0, dret = 0;
  gzFile fp;
	kstream_t *ks;
	kstring_t *str;
  assemblies_array_t *a = (assemblies_array_t*)calloc(1, sizeof(assemblies_array_t));
  khiter_t k, k2;

  kv_init(*a);
  str = calloc(1, sizeof(kstring_t));

  fp = gzopen(counts_file, "r");
  if(!fp) { fprintf(stderr, "Failed to open %s\n", counts_file); exit(EXIT_FAILURE); }

  ks = ks_init(fp);
  // Skip headers
  ks_getuntil(ks, KS_SEP_LINE, str, &dret);
  while (ks_getuntil(ks, 0, str, &dret) >= 0) {
    assembly_t *assembly = (assembly_t*)calloc(1, sizeof(assembly_t));
    assembly->ref_kmer_id = nb_kmers;
    assembly->seq         = ks_release(str);
    assembly->nb_merged_kmers = 1;
    if(dret != '\n' && ks_getuntil(ks, 0, str, &dret) > 0 && isdigit(str->s[0])) {
      assembly->pvalue = atof(str->s);
      kv_push(assembly_t*, *a, assembly);
    } else {
      assembly_destroy(assembly);
    }
    // skip the rest of the line
		if (dret != '\n') while ((dret = ks_getc(ks)) > 0 && dret != '\n');
    nb_kmers++;
  }
  ks_destroy(ks);
  gzclose(fp);

  fprintf(stderr, "%d k-mers loaded\n", nb_kmers);
  fprintf(stderr, "Assembling k-mers\n");

  for(int i = k_length - 1; i >= MIN_ASSEMBLY_K; i--) {
    int has_merge_assemblies = 1;
    while(has_merge_assemblies) {
      has_merge_assemblies = 0;

      khash_t(kmers) *left_h  = kh_init(kmers);
      khash_t(kmers) *right_h = kh_init(kmers);
      assemblies_array_t *new_a = (assemblies_array_t*)calloc(1, sizeof(assemblies_array_t));
      kv_init(*new_a);

      fprintf(stderr, "Merging sequences with k = %d\n", i);

      fprintf(stderr, "Creating kmer-end indexes\n");

      // First we index the start and ends of each assembly
      for(int j = 0; j < kv_size(*a); j++) {
        assembly_t *assembly = kv_A(*a,j);
        if(strlen(assembly->seq) <= i) continue;
        uint64_t left_kmer  = dna_to_int(assembly->seq, i);
        uint64_t right_kmer = dna_to_int(&assembly->seq[strlen(assembly->seq) - i], i);

        k = kh_get(kmers, left_h, left_kmer);
        if(k == kh_end(left_h)) {
          k = kh_put(kmers, left_h, left_kmer, &dret);
          kh_value(left_h, k) = j + 1;
        } else {
          kh_value(left_h, k) = 0;
        }

        k = kh_get(kmers, right_h, right_kmer);
        if(k == kh_end(right_h)) {
          k = kh_put(kmers, right_h, right_kmer, &dret);
          kh_value(right_h, k) = j + 1;
        } else {
          kh_value(right_h, k) = 0;
        }
      }

      fprintf(stderr, "Merging sequences\n");
      // Now we try to merge assemblies
      for(k = kh_begin(right_h); k != kh_end(right_h); ++k) {
        if(!kh_exist(right_h, k)) continue;
        if(kh_value(right_h, k) == 0) continue;

        assembly_t *left_assembly = kv_A(*a, kh_value(right_h, k) - 1);

        if(!left_assembly) continue;

        k2 = kh_get(kmers, left_h, kh_key(right_h, k));

        if(k2 != kh_end(left_h) && kh_value(left_h, k2) != 0 && kh_value(left_h, k2) != kh_value(right_h, k)) {

          has_merge_assemblies = 1;
          // Merge the two sequences into a new assembly
          assembly_t *right_assembly = kv_A(*a, kh_value(left_h, k2) - 1);

          if(!right_assembly) continue;

          assembly_t *assemblies_merge, *removed_assembly;
          // Keep the k-mers with lowest pvalue to represent the assembly
          if(left_assembly->pvalue < right_assembly->pvalue) {
            assemblies_merge = left_assembly;
            removed_assembly = right_assembly;
          } else {
            assemblies_merge = right_assembly;
            removed_assembly = left_assembly;
          }
          char *merge_seq = malloc(strlen(right_assembly->seq) + strlen(left_assembly->seq) - i + 1);
          strcpy(merge_seq, left_assembly->seq);
          strcat(merge_seq, &right_assembly->seq[i]);

          free(right_assembly->seq);
          free(left_assembly->seq);
          right_assembly->seq = NULL;
          left_assembly->seq  = NULL;
          assemblies_merge->nb_merged_kmers = left_assembly->nb_merged_kmers + right_assembly->nb_merged_kmers;
          assemblies_merge->seq = merge_seq;
          assembly_destroy(removed_assembly);

          kv_push(assembly_t*, *new_a, assemblies_merge);
          kv_A(*a, kh_value(right_h, k) - 1) = NULL;
          kv_A(*a, kh_value(left_h, k2) - 1) = NULL;
        }
      }

      fprintf(stderr, "Add un-merged sequences\n");
      // Add assemblies that were not merged
      for(int j = 0; j < kv_size(*a); j++) {
        assembly_t *assembly = kv_A(*a,j);
        if(assembly) {
          kv_push(assembly_t*, *new_a, assembly);
        }
      }
      fprintf(stderr, "%zu assemblies after merging\n", kv_size(*new_a));
      // Delete previous assebly array and replace it with the new one
      kv_destroy(*a);
      a = new_a;
      // Delete hashes
      kh_destroy(kmers, left_h);
      kh_destroy(kmers, right_h);
    }
  }

  // Sort assemblies by ref_kmer_id
  qsort(a->a, kv_size(*a), sizeof(a->a[0]), cmp_assembly);

  fp = gzopen(counts_file, "r");
  if(!fp) { fprintf(stderr, "Failed to open %s\n", counts_file); exit(EXIT_FAILURE); }

  ks = ks_init(fp);
  nb_kmers = 0;
  int j = 0;
  // Print headers
  ks_getuntil(ks, KS_SEP_LINE, str, &dret);
  fprintf(stdout, "nb_merged_kmers\tcontig\t%s\n", str->s);
  while (ks_getuntil(ks, KS_SEP_LINE, str, &dret) >= 0 && j < kv_size(*a)) {
    if(nb_kmers == a->a[j]->ref_kmer_id) {
      assembly_t *assembly = kv_A(*a,j);
      fprintf(stdout, "%d\t%s\t%s\n",assembly->nb_merged_kmers,assembly->seq,str->s);
      assembly_destroy(assembly);
      kv_A(*a, j) = NULL;
      j++;
    }
    nb_kmers++;
  }
  gzclose(fp);
  ks_destroy(ks);
  free(str->s); free(str);

  kv_destroy(*a);
  //free(a);
	return 0;
}
