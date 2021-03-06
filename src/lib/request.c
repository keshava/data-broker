/*
 * Copyright © 2018-2020 IBM Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#ifdef __APPLE__
#include <stdlib.h>
#else
#include <malloc.h>
#endif
#include <string.h>
#include <errno.h>

#include "logutil.h"
#include "libdatabroker.h"
#include "libdatabroker_int.h"


dbrRequestContext_t* dbrCreate_request_ctx(dbBE_Opcode op,
                                           dbrName_space_t *cs,
                                           DBR_Group_t group,
                                           dbrName_space_t *dst_cs,
                                           DBR_Group_t dst_group,
                                           int sge_count,
                                           dbBE_sge_t *sge,
                                           int64_t *rc,
                                           DBR_Tuple_name_t tuple_name,
                                           DBR_Tuple_template_t match_template,
                                           DBR_Tag_t tag )
{
  if(( cs == NULL )||(cs->_db_name == NULL))
    return NULL;

  if( tag == DB_TAG_ERROR )
    return NULL;

  if(( sge_count < 0 ) ||
      (( sge_count>0 ) && ( sge == NULL )) ||
      (( sge_count==0 ) && ( sge != NULL )) )
    return NULL;

  if( op >= DBBE_OPCODE_MAX )
    return NULL;

  DBR_Tuple_name_t key = tuple_name;

  dbBE_sge_t temp_sge[2];
  switch( op )
  {
    case DBBE_OPCODE_MOVE:
      // for the move cmd, we'll put the destination cs and group into the SGE/value
      sge_count = 2;
      temp_sge[0].iov_base = dst_cs->_be_ns_hdl;
      temp_sge[0].iov_len = sizeof( dst_cs->_be_ns_hdl );
      temp_sge[1].iov_base = dst_group;
      temp_sge[1].iov_len = sizeof( DBR_Group_t );
      sge = temp_sge;
      break;
    case DBBE_OPCODE_ITERATOR:
      sge_count = 1;
      temp_sge[0].iov_base = tuple_name; // returned key
      temp_sge[0].iov_len = DBR_MAX_KEY_LEN;
      key = (char*)(*rc);  // the key becomes the iterator ptr
      sge = temp_sge;
      break;
    default:
      break;
  }

  dbrRequestContext_t *req = (dbrRequestContext_t*)calloc( 1, sizeof( dbrRequestContext_t ) + sge_count * sizeof(dbBE_sge_t) );
  if( req == NULL )
    return NULL;

  req->_req._ns_hdl = cs->_be_ns_hdl;
  req->_req._group = group;
  req->_req._key = key;
  req->_req._match = match_template;
  req->_req._sge_count = sge_count;
  req->_req._opcode = op;
  req->_req._user = req; // self-reference so that we find the right request after completion
  if( sge )
    memcpy( req->_req._sge, sge, sge_count * sizeof( dbBE_sge_t ) );
  // req->_cpl just keep it all 0 after memset
  req->_cpl._status = DBR_SUCCESS;
  req->_status = dbrSTATUS_PENDING;
  req->_ctx = cs;
  req->_be_request_hdl = NULL;
  req->_rc = rc;
  req->_tag = tag;

  return req;
}

DBR_Errorcode_t dbrDestroy_request( dbrRequestContext_t *rctx )
{
  if( rctx == NULL )
    return DBR_ERR_INVALID;
  memset( rctx, 0, sizeof( dbrRequestContext_t ) + rctx->_req._sge_count * sizeof(dbBE_sge_t) );
  free( rctx );
  return DBR_SUCCESS;
}

dbrRequestContext_t* dbrCreate_request_chain( dbBE_Opcode op,
                                              dbrName_space_t *ns,
                                              DBR_Group_t group,
                                              dbrName_space_t *dst_ns,
                                              DBR_Group_t dst_group,
                                              dbrDA_Request_chain_t *requests,
                                              DBR_Tuple_template_t match_template,
                                              int flags,
                                              DBR_Tag_t tag )
{
  dbrRequestContext_t *prev = NULL;
  dbrRequestContext_t *head = NULL;

  dbrDA_Request_chain_t *item = requests;
  while( item != NULL )
  {
    dbrRequestContext_t *ctx;
    ctx = dbrCreate_request_ctx( op,
                                 ns,
                                 group,
                                 dst_ns,
                                 dst_group,
                                 item->_sge_count,
                                 item->_value_sge,
                                 item->_ret_size,
                                 item->_key,
                                 match_template,
                                 tag );
    if( ctx == NULL )
      goto error;

    ctx->_req._flags = flags;

    // chain the request contexts
    if( prev != NULL )
      prev->_next = ctx;
    else
      head = ctx; // remember the first one

    prev = ctx;
    // some basic sanity check because we're processing a data structure from the plugin
    if( item == item->_next )
    {
      LOG( DBG_ERR, stderr, "Request chain error detected. Self-referencing\n");
      goto error;
    }
    item = item->_next;
  }

  return head;
error:
  dbrDestroy_request_chain( head );
  return NULL;
}

DBR_Errorcode_t dbrDestroy_request_chain( dbrRequestContext_t *chain )
{
  while( chain != NULL )
  {
    dbrRequestContext_t *tmp = chain;
    if( chain->_next != chain )
      chain = chain->_next;
    else
      chain = NULL;
    dbrDestroy_request( tmp );
  }
  return DBR_SUCCESS;
}

DBR_Tag_t dbrInsert_request( dbrName_space_t *cs, dbrRequestContext_t *rctx )
{
  if( cs == NULL
      || rctx == NULL || cs->_reverse == NULL )
    return DB_TAG_ERROR;

  dbrRequestContext_t** cs_wq = cs->_reverse->_cs_wq;

  DBR_Tag_t tag = rctx->_tag;

  if( dbrValidateTag( rctx, tag ) != DBR_SUCCESS )
    return DB_TAG_ERROR;

#ifdef DBR_INTTAG
  unsigned int tag_idx = tag;

#else
  dbrRequestContext_t** p_rctx = (dbrRequestContext_t**)tag;
  if( p_rctx < cs_wq
      || &cs_wq[ dbrMAX_TAGS ] <= p_rctx )
    return DB_TAG_ERROR;

  unsigned int tag_idx = p_rctx - cs_wq;
#endif

  if( cs_wq[ tag_idx ] != NULL )
  {
    if( cs_wq[ tag_idx ]->_status != dbrSTATUS_CLOSED )
    {
      return DB_TAG_ERROR;
    }
    free( cs_wq[ tag_idx ] );
    cs_wq[ tag_idx ] = NULL;
  }

  cs_wq[ tag_idx ] = rctx;
  return tag;
}

DBR_Errorcode_t dbrRemove_request( dbrName_space_t *cs, dbrRequestContext_t *rctx )
{
  if( cs == NULL
      || rctx == NULL || cs->_reverse == NULL )
    return DBR_ERR_INVALID;

  dbrRequestContext_t** cs_wq = cs->_reverse->_cs_wq;

  DBR_Tag_t tag = rctx->_tag;

  if( dbrValidateTag( rctx, tag ) != DBR_SUCCESS )
    return DBR_ERR_TAGERROR;

#ifdef DBR_INTTAG
  unsigned int tag_idx = tag;

#else
  dbrRequestContext_t** p_rctx = (dbrRequestContext_t**)tag;
  if( p_rctx < cs_wq
      || &cs_wq[ dbrMAX_TAGS ] <= p_rctx )
    return DBR_ERR_TAGERROR;

  unsigned int tag_idx = p_rctx - cs_wq;
#endif

  DBR_Errorcode_t rc = DBR_ERR_HANDLE; // assume the rctx-handle is not in the WQ-list until we actually find it
  while( cs_wq[ tag_idx ] != NULL )
  {
    dbrRequestContext_t *chain = cs_wq[ tag_idx ]->_next;
    if(( chain != NULL )&&( chain->_tag != tag ))
    {
      printf( "BUG: chained request with different tag.\n" );
      return DBR_ERR_INVALID;
    }
    // todo: if there's a backend handle reference, we might have to clean it up
    if( cs_wq[ tag_idx ]->_be_request_hdl != NULL )
      LOG( DBG_VERBOSE, stderr, "TODO: cleanup backend handle?\n" );

    if( rctx == cs_wq[ tag_idx ] )
    {
      LOG( DBG_VERBOSE, stdout, "Found the requested context\n" );
      rc = DBR_SUCCESS;
    }

    // todo: to prevent request deletion caused by an invalid rctx, move the requests to tmp deletion queue instead until we're sure the correct stuff is deleted
    dbrDestroy_request( cs_wq[ tag_idx ] );
    cs_wq[ tag_idx ] = chain;
  }

  cs_wq[ tag_idx ] = NULL;
  return rc;
}

DBR_Request_handle_t dbrPost_request_ext( dbrRequestContext_t *rctx, const int with_trigger )
{
  if( rctx == NULL || rctx->_ctx == NULL || rctx->_ctx->_reverse == NULL )
    return NULL;

  dbrBackend_t *be = rctx->_ctx->_reverse->_be_ctx;
  dbrRequestContext_t *chain = rctx;
  int rcount = 0;
  while( chain != NULL )
  {
    if( dbrValidateTag( chain, chain->_tag ) != DBR_SUCCESS )
      return NULL;

    if( chain->_ctx->_reverse->_cs_wq[ chain->_tag ] == NULL )
    {
      LOG( DBG_ERR, stderr, "Request not inserted in namespace request list.\n" );
      return NULL;
    }

    if( chain->_cpl._status == DBR_ERR_INPROGRESS )
    {
      LOG( DBG_ERR, stderr, "Request already in progress\n");
      return NULL;
    }
    chain->_cpl._status = DBR_ERR_INPROGRESS;
    chain->_status = dbrSTATUS_PENDING;

    rcount = ((rcount+1) % 128 );
    int trigger = (( chain->_next == NULL ) && ( with_trigger )) || ( rcount == 0 );
    dbBE_Request_handle_t be_handle = NULL;
    do {
      be_handle = be->_api->post( be->_context, &chain->_req, trigger );
    } while(( be_handle == NULL ) && ( errno == EAGAIN ));

      if( be_handle == NULL )
      goto error;
    chain->_be_request_hdl = be_handle;

    chain = chain->_next;
  }

  return (DBR_Request_handle_t)rctx;

error:
  return NULL;
}

DBR_Request_handle_t dbrPost_request( dbrRequestContext_t *rctx )
{
  return dbrPost_request_ext( rctx, 1 );
}
