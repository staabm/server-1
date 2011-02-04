/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2010 Tokutek Inc.  All rights reserved."
#ident "$Id$"

#include "test.h"
#include "toku_pthread.h"
#include "toku_atomic.h"
#include <db.h>
#include <sys/stat.h>
#include "ydb-internal.h"

#include "test_kv_gen.h"

/****************************************************************************************
 *
 * Test sequence is run four times, two in outer loop, two in inner loop
 * Outer loop is for default or small node and cachetable sizes,
 * inner loop is for insert and delete.
 *
 * open dbs
 * read and verify first n rows of primary, a few interspersed rows of secondaries  (n is very small so only a few nodes of secondaries are upgraded, even with prefetch)
 * close dbs  (dictionaries now partially upgraded)
 * open dbs
 * read and verify a few more rows of primary, a few more interspersed rows of secondaries
 * close dbs  (some more nodes now upgraded)
 * open dbs
 * if (insert test)
 *   insert at end of primary and interspersed in secondary dictionaries
 * else (delete test)
 *   delete from beginning of primary and interspersed in secondary dictionaries
 * close dbs
 * open dbs
 * verify all rows (including newly inserted ones)
 * close dbs
 *
 */

DB_ENV *env;
enum {MAX_NAME=128};
int NUM_DBS=5;
int NUM_ROWS=100000;
int CHECK_RESULTS=0;
int SRC_VERSION = 4;
int littlenode = 0;

#define OLDDATADIR "../../../../tokudb.data/"

char *env_dir = ENVDIR; // the default env_dir.
char *db_v5_dir = "dir.preload-db.c.tdb";
char *db_v4_dir        = OLDDATADIR "env_preload.4.2.0.cleanshutdown";
char *db_v4_dir_node4k = OLDDATADIR "env_preload.4.2.0.node4k.cleanshutdown";


enum {ROWS_PER_TRANSACTION=10000};

static int idx[MAX_DBS];

typedef enum {insert, delete} test_type;

static void
open_dbs(DB **dbs) {
    int r;
    DBT desc;
    dbt_init(&desc, "foo", sizeof("foo"));
    char name[MAX_NAME*2];
    
    for(int i=0;i<NUM_DBS;i++) {
	idx[i] = i;
	r = db_create(&dbs[i], env, 0);                                                                       CKERR(r);
	r = dbs[i]->set_descriptor(dbs[i], 1, &desc);                                                         CKERR(r);
	dbs[i]->app_private = &idx[i];
	snprintf(name, sizeof(name), "db_%04x", i);
	r = dbs[i]->open(dbs[i], NULL, name, NULL, DB_BTREE, DB_CREATE, 0666);                                CKERR(r);
    }
}


static void
close_dbs(DB **dbs) {
    for(int i=0;i<NUM_DBS;i++) {
	int r = dbs[i]->close(dbs[i], 0);                                                                          CKERR(r);
	dbs[i] = NULL;
    }
}


static void upgrade_test_4(DB **dbs, test_type test_to_do) {
    int r;
    int n = 4;  // number of rows to check to partially upgrade dictionary
    char * msg;
    if (test_to_do == insert)
	msg = "insert";
    else if (test_to_do == delete)
	msg = "delete";
    else assert(0);
    

    // open the DBS
    open_dbs(dbs);

    // check first few rows of primary, some (pseudo)random rows of secondaries
    {
	check_results(env, dbs, NUM_DBS, n);
	if (verbose)
	    printf("First %d rows checked, now close and reopen\n", n);
    }

    // close and reopen
    close_dbs(dbs);
    open_dbs(dbs);

    // check first few rows of primary, some (pseudo)random rows of secondaries
    {
	n *= 2;
	check_results(env, dbs, NUM_DBS, n);
	if (verbose)
	    printf("\nFirst %d rows checked, now %s some rows\n", n, msg);
    }

    // close and reopen
    close_dbs(dbs);
    open_dbs(dbs);

    // insert or delete some rows
    DB_TXN    *txn;
    DBT skey, sval;
    DBT key, val;
    dbt_init_realloc(&key);
    dbt_init_realloc(&val);

    unsigned int k, v;
    if ( verbose ) { 
	printf("%s some rows\n", msg);
	fflush(stdout); 
    }
    int num_rows_to_modify, base;
    if (test_to_do == insert) {
	num_rows_to_modify = NUM_ROWS;
	base = NUM_ROWS;  // insert after existing rows in primary
    }
    else if (test_to_do == delete) {
	num_rows_to_modify = 2*n;
	base = 0;  // delete some rows from primary
    }
    else assert(0);
    int outer_loop_num = ( num_rows_to_modify <= ROWS_PER_TRANSACTION ) ? 1 : (num_rows_to_modify / ROWS_PER_TRANSACTION);
    for(int x=0;x<outer_loop_num;x++) {
        r = env->txn_begin(env, NULL, &txn, 0);                                                              CKERR(r);
        for(int i=1; (i<=ROWS_PER_TRANSACTION && i<=num_rows_to_modify); i++) {
            k = i + (x*ROWS_PER_TRANSACTION) + base;
            v = generate_val(k, 0);
            dbt_init(&skey, &k, sizeof(unsigned int));
            dbt_init(&sval, &v, sizeof(unsigned int));

            for(int db = 0;db < NUM_DBS;db++) {
                put_multiple_generate(dbs[db], // dest_db
                                      NULL, // src_db, ignored
                                      &key, &val, // 
                                      &skey, &sval, // src_key, src_val
                                      NULL); // extra, ignored
		if (test_to_do == insert) {
		    r = dbs[db]->put(dbs[db], txn, &key, &val, 0);
		    CKERR(r);
		}
		else if (test_to_do == delete) {
		    r = dbs[db]->del(dbs[db], txn, &key, 0);
		    CKERR(r);
		}
		else assert(0);

                if (key.flags == 0) { dbt_init_realloc(&key); }
                if (val.flags == 0) { dbt_init_realloc(&val); }
            }
        }
        r = txn->commit(txn, 0);                                                                             CKERR(r);
        if ( verbose ) {printf(".");fflush(stdout);}
    }
    if ( key.flags ) { toku_free(key.data); key.data = NULL; }
    if ( val.flags ) { toku_free(val.data); key.data = NULL; }

    // close
    close_dbs(dbs);
    
    // open
    open_dbs(dbs);

    // read and verify all rows
    {
        if ( verbose ) {printf("\nchecking");fflush(stdout);}
	if (test_to_do == insert)
	    check_results(env, dbs, NUM_DBS, NUM_ROWS * 2);
	else if (test_to_do == delete)
	    check_results_after_row_n(env, dbs, NUM_DBS, NUM_ROWS, num_rows_to_modify);
	else assert(0);
        if ( verbose) {printf("\ndone\n");fflush(stdout);}
    }
    // close
    {
        for(int i=0;i<NUM_DBS;i++) {
            r = dbs[i]->close(dbs[i], 0);                                                                         CKERR(r);
            dbs[i] = NULL;
        }
    }
}

static void setup(void) {
    int r;
    int len = 256;
    char syscmd[len];
    char * src_db_dir;

    if ( SRC_VERSION == 4 ) {
	if (littlenode)
	    src_db_dir = db_v4_dir_node4k;
	else
	    src_db_dir = db_v4_dir;
    }
    else if ( SRC_VERSION == 5 ) {
        src_db_dir = db_v5_dir;
    }
    else {
        fprintf(stderr, "unsupported TokuDB version %d to upgrade\n", SRC_VERSION);
        assert(0);
    }

    r = snprintf(syscmd, len, "rm -rf %s", env_dir);
    assert(r<len);
    r = system(syscmd);                                                                                  
    CKERR(r);
    
    r = snprintf(syscmd, len, "cp -r %s %s", src_db_dir, env_dir);
    assert(r<len);
    r = system(syscmd);                                                                                 
    CKERR(r);
    generate_permute_tables();

}

static void run_test(test_type test_to_do) 
{
    int r;

    r = db_env_create(&env, 0);                                                                               CKERR(r);
    if (littlenode) {
	r = env->set_cachesize(env, 0, 512*1024, 1);                                                              CKERR(r); 
    }
    r = env->set_redzone(env, 0);                                                                             CKERR(r);
    int envflags = DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL | DB_INIT_TXN | DB_CREATE | DB_PRIVATE;
    r = env->open(env, env_dir, envflags, S_IRWXU+S_IRWXG+S_IRWXO);                                            CKERR(r);
    env->set_errfile(env, stderr);
    r = env->checkpointing_set_period(env, 5);                                                                CKERR(r);

    DB **dbs = (DB**)toku_malloc(sizeof(DB*) * NUM_DBS);
    assert(dbs != NULL);

    // --------------------------
    upgrade_test_4(dbs, test_to_do);
    // --------------------------

    if (verbose >= 2)
	print_engine_status(env);
    r = env->close(env, 0);                                                                                   CKERR(r);
    toku_free(dbs);

}

// ------------ infrastructure ----------
static void do_args(int argc, char * const argv[]);

int test_main(int argc, char * const *argv) {
    do_args(argc, argv);
    littlenode = 0;
    setup();
    run_test(insert);
    setup();
    run_test(delete);
    if (SRC_VERSION == 4) {
	if (verbose)
	    printf("Now repeat test with small nodes and small cache.\n");
	littlenode = 1;  // 4k nodes, small cache
	setup();
	run_test(insert);
	setup();
	run_test(delete);
    }
    return 0;
}

static void do_args(int argc, char * const argv[]) {
    int resultcode;
    char *cmd = argv[0];
    argc--; argv++;
    
    while (argc>0) {
	if (strcmp(argv[0], "-v")==0) {
	    verbose++;
	} else if (strcmp(argv[0],"-q")==0) {
	    verbose--;
	    if (verbose<0) verbose=0;
        } else if (strcmp(argv[0], "-h")==0) {
	    resultcode=0;
	do_usage:
	    fprintf(stderr, "Usage: -h -c -d <num_dbs> -r <num_rows> %s\n", cmd);
	    exit(resultcode);
        } else if (strcmp(argv[0], "-d")==0) {
            argc--; argv++;
            NUM_DBS = atoi(argv[0]);
            if ( NUM_DBS > MAX_DBS ) {
                fprintf(stderr, "max value for -d field is %d\n", MAX_DBS);
                resultcode=1;
                goto do_usage;
            }
        } else if (strcmp(argv[0], "-r")==0) {
            argc--; argv++;
            NUM_ROWS = atoi(argv[0]);
        } else if (strcmp(argv[0], "-c")==0) {
            CHECK_RESULTS = 1;
        } else if (strcmp(argv[0], "-V")==0) {
            argc--; argv++;
            SRC_VERSION = atoi(argv[0]);
	} else {
	    fprintf(stderr, "Unknown arg: %s\n", argv[0]);
	    resultcode=1;
	    goto do_usage;
	}
	argc--;
	argv++;
    }
}
