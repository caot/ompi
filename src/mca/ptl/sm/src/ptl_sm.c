/*
 * $HEADER$
 */

#include <string.h>
#include "util/output.h"
#include "util/if.h"
#include "mca/pml/pml.h"
#include "mca/ptl/ptl.h"
#include "mca/pml/base/pml_base_sendreq.h"
#include "mca/pml/base/pml_base_recvreq.h"
#include "mca/ptl/base/ptl_base_header.h"
#include "mca/ptl/base/ptl_base_sendfrag.h"
#include "mca/ptl/base/ptl_base_recvfrag.h"
#include "mca/base/mca_base_module_exchange.h"
#include "ptl_sm.h"
#include "util/sys_info.h"
#include "mca/ptl/sm/src/ptl_sm_peer.h"
#include "mca/ptl/sm/src/ptl_sm_sendfrag.h"


mca_ptl_sm_t mca_ptl_sm = {
  {
      /* RICH: I have no idea what should be here -- you need to check
         all these values. --JMS */
    &mca_ptl_sm_component.super,
    0, /* max size of request cache */
    sizeof(mca_ptl_sm_send_frag_t), /* bytes required by ptl for a request */
    0, /* max size of first fragment */
    0, /* min fragment size */
    0, /* max fragment size */
    1, /* exclusivity */
    0, /* latency */
    0, /* bandwidth */
    MCA_PTL_PUT,  /* ptl flags */
    mca_ptl_sm_add_procs,
    mca_ptl_sm_del_procs,
    mca_ptl_sm_finalize,
    mca_ptl_sm_send,
    NULL,
    NULL, /* RICH: need matched function here */
    NULL, /* RICH: need request_init function here */
    NULL /* RICH: need request_fini function here */
    }
};


int mca_ptl_sm_add_procs(
    struct mca_ptl_base_module_t* ptl, 
    size_t nprocs, 
    struct ompi_proc_t **procs, 
    struct mca_ptl_base_peer_t **peers,
    ompi_bitmap_t* reachability)
{
    int i,proc,my_smp_rank,return_code=OMPI_SUCCESS;
    size_t size,len,my_len,n_local_procs;
    mca_ptl_sm_exchange_t **sm_proc_info;
    ompi_proc_t* my_proc; /* pointer to caller's proc structure */
    mca_ptl_sm_t *ptl_sm;

    /* initializion */
    for(i=0 ; i < nprocs ; i++ ) {
        peers[i]=NULL;
    }
    ptl_sm=(mca_ptl_sm_t *)ptl;

    /* allocate array to hold setup shared memory from all
     * other procs */
    sm_proc_info=(mca_ptl_sm_exchange_t **)
        malloc(nprocs*sizeof(mca_ptl_sm_exchange_t *));
    if( NULL == sm_proc_info ){
        return_code=OMPI_ERR_OUT_OF_RESOURCE;
        goto CLEANUP;
    }

    /* get pointer to my proc structure */
    my_proc=ompi_proc_local();
    if( NULL == my_proc ) {
        return_code=OMPI_ERR_OUT_OF_RESOURCE;
        goto CLEANUP;
    }
    my_len=strlen(ompi_system_info.nodename);

    /* Get unique host identifier for each process in the list,
     * and idetify procs that are on this host.  Add procs on this
     * host to shared memory reachbility list.  Also, get number
     * of local procs in the prcs list. */
    n_local_procs=0;
    for( proc=0 ; proc < nprocs; proc++ ) {
        /* don't compare with self */
        if( my_proc == procs[proc] ) {
            ptl_sm->my_smp_rank=n_local_procs;
            n_local_procs++;
            continue;
        }
        return_code = mca_base_modex_recv(
                &mca_ptl_sm_component.super.ptlm_version, procs[proc],
                (void**)(&(sm_proc_info[proc])), &size);
        if(return_code != OMPI_SUCCESS) {
            ompi_output(0, "mca_ptl_sm_add_procs: mca_base_modex_recv: failed with return value=%d", return_code);
            goto CLEANUP;
        }

       /* for zero length, just continue - comparison is meaningless*/
       if( 0 >= size ) {
           continue;
       }

       /* check to see if this proc is on my host */
       len=strlen((char *)(sm_proc_info[proc]));
       if( len == my_len ) {
           if( 0 == strncmp(ompi_system_info.nodename,
                       (char *)(sm_proc_info[proc]),len) ) {

               /* initialize the peers information */
               peers[proc]=malloc(sizeof(struct mca_ptl_base_peer_t));
               if( NULL == peers[proc] ){
                   return_code=OMPI_ERR_OUT_OF_RESOURCE;
                   goto CLEANUP;
               }
               peers[proc]->peer_smp_rank=n_local_procs+
                   ptl_sm->num_smp_procs;
               n_local_procs++;

               /* add this proc to shared memory accessability list */
               return_code=ompi_bitmap_set_bit(reachability,proc);
               if( OMPI_SUCCESS != return_code ){
                   goto CLEANUP;
               }
           }
       }

    }

    /* make sure that my_smp_rank has been defined */
    if(-1 == ptl_sm->my_smp_rank){
        return_code=OMPI_ERROR;
        goto CLEANUP;
    }

    /* set local proc's smp rank in the peers structure for
     * rapid access */
    for( proc=0 ; proc < nprocs; proc++ ) {
        if(NULL != peers[proc] ) {
            peers[proc]->my_smp_rank=ptl_sm->my_smp_rank;
        }
    }

    /* Allocate a fixed size pointer array for the 2-D Shared memory queues. 
     * Excess slots will be allocated for future growth.  One could
     * make this array growable, but then one would need to uses mutexes
     * for any access to these queues to ensure data consistancy when
     * the array is grown */

    /* Note:  Need to make sure that proc 0 initializes control
     * structures before any of the other procs can progress */

    /* Initizlize queue data structures 
     *   - proc with lowest local rank does this
     *   - all the rest of the procs block until the queues are
     *     initialized
     *   - initial queue size is zero */

    /* free local memory */
    if(sm_proc_info){
       /* free the memory allocated by mca_base_modex_recv */
        for( proc=0 ; proc < nprocs; proc++ ) {
            if(sm_proc_info[proc]){
                free(sm_proc_info[proc]);
            }
        }
        free(sm_proc_info);
    }

    /* update the local smp process count */
    ptl_sm->num_smp_procs+=n_local_procs;

CLEANUP:
    if(sm_proc_info){
        free(sm_proc_info);
    }

    return return_code;
}


int mca_ptl_sm_del_procs(
    struct mca_ptl_base_module_t* ptl, 
    size_t nprocs,
    struct ompi_proc_t **procs, 
    struct mca_ptl_base_peer_t **peers)
{
    return OMPI_SUCCESS;
}


int mca_ptl_sm_finalize(struct mca_ptl_base_module_t* ptl)
{
    return OMPI_SUCCESS;
}


int mca_ptl_sm_request_alloc(struct mca_ptl_base_module_t* ptl, struct mca_pml_base_send_request_t** request)
{
    return OMPI_SUCCESS;
}


void mca_ptl_sm_request_return(struct mca_ptl_base_module_t* ptl, struct mca_pml_base_send_request_t* request)
{
}


/*
 *  Initiate a send. If this is the first fragment, use the fragment
 *  descriptor allocated with the send requests, otherwise obtain
 *  one from the free list. Initialize the fragment and foward
 *  on to the peer.
 */

int mca_ptl_sm_send(
    struct mca_ptl_base_module_t* ptl,
    struct mca_ptl_base_peer_t* ptl_peer,
    struct mca_pml_base_send_request_t* sendreq,
    size_t offset,
    size_t size,
    int flags)
{
    return OMPI_SUCCESS;
}


/*
 *  A posted receive has been matched - if required send an
 *  ack back to the peer and process the fragment.
 */

void mca_ptl_sm_matched(
    mca_ptl_base_module_t* ptl,
    mca_ptl_base_recv_frag_t* frag)
{
}


