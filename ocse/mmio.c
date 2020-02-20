/*
 * Copyright 2014,2018 International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Description: mmio.c
 *
 *  This file contains the code for MMIO access to the AFU including the
 *  AFU configuration space.  Only one MMIO access is legal at a time.  So each
 *  client only tracks up to one mmio_access at a time.  However, since a
 *  "directed mode" AFU may have multiple clients attached the mmio struct
 *  tracks multiple mmio accesses with the element "list."  As MMIO requests
 *  are received from clients they are added to the list and handled in FIFO
 *  order.  The _add_event() function places each new MMIO event on the list
 *  as they are received from a client.  The ocl code will periodically call
 *  send_mmio() which will drive the oldest pending MMIO command event to the AFU.
 *  That event is put in PENDING state which blocks the OCL from sending any
 *  further MMIO until this MMIO event completes.  When the ocl code detects
 *  the MMIO response it will call handle_ap_resp().  This function moves
 *  the list head to the next event so that the next MMIO request can be sent.
 *  However, the event still lives and the client will still point to it.  When
 *  the ocl code next calls handle_mmio_done for that client it will return the
 *  acknowledge as well as any data to the client.  At that point the event
 *  memory will be freed.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <math.h>


#include "mmio.h"
#include "client.h"
#include "../common/debug.h"
// #include "ocl.h"

// Initialize MMIO tracking structure
struct mmio *mmio_init(struct AFU_EVENT *afu_event, int timeout, char *afu_name,
		       FILE * dbg_fp, uint8_t dbg_id)
{
	struct mmio *mmio = (struct mmio *)calloc(1, sizeof(struct mmio));
	if (!mmio)
		return mmio;
	mmio->afu_event = afu_event;
	mmio->list = NULL;
	mmio->afu_name = afu_name;
	mmio->dbg_fp = dbg_fp;
	mmio->dbg_id = dbg_id;
	mmio->timeout = timeout;
	return mmio;
}

// Add new MMIO event
static struct mmio_event *_add_event(struct mmio *mmio, struct client *client,
				     uint32_t rnw, uint32_t dw, int global, uint64_t addr,
				     uint32_t cfg, uint64_t data)
{
	struct mmio_event *event;
	struct mmio_event **list;
	uint8_t fcn; 
	uint8_t afuid;
	uint16_t context;

  debug_msg( "_add_event:" );
	// Add new event in IDLE state
        event = (struct mmio_event *)calloc(1, sizeof(struct mmio_event));
	if (!event)
		return event;
	event->rnw = rnw;
	event->dw = dw;
	event->size = 0;  // part of the new fields
	event->size_received = 0;  // part of the new fields
	event->be_valid = 0;  // part of the new fields
	event->data = NULL;
	event->be = 0;
	event->cmd_dL = 0;
	event->cmd_dP = 0;
	if (client == NULL)  {
	  // is this case where cfg = 1, that is, we want to read config space?
	  // yes, when we do mmios to config space, we force client to null
	  event->cmd_PA = addr;
	} else {
	  // for OpenCAPI, the mmio space is split into global and per pasid
	  // the global parm controls how we adjust the offset prior to adding the event
	  //   global = 1 means we offset based on the global mmio offset from the configuration
	  //   global = 0 means we want to send the offset adjusted by the per pasid mmio offset, per pasid mmio stride, and client index
	  //   for now, we are assuming the client index (context) maps directly to a pasid.  
	  //        we could be more creative and relocate the pasid base and pasid length supported to 
	  //        provide more verification coverage
	  fcn = client->fcn;
	  afuid = client->afuid;

	  if (global == 1) {
	    // bar0 + global mmio offset + offset
	    event->cmd_PA = mmio->fcn_cfg_array[fcn]->bar0 + 
	                    mmio->fcn_cfg_array[fcn]->afu_cfg_array[afuid]->global_mmio_offset + 
	                    addr;
	    debug_msg( "global mmio to fcn/afu %d/%d : offset (0x%016lx) + addr (0x%016lx) = cmd_pa (0x%016lx)", 
		       fcn, afuid, mmio->fcn_cfg_array[fcn]->afu_cfg_array[afuid]->global_mmio_offset, addr, event->cmd_PA );
	  } else {
	    // bar0 + per pasid mmio offset + (client context * stride) + offset
	    // TODO offset is NOW 64b, comprised of offset_high & offset_low
	    event->cmd_PA = mmio->fcn_cfg_array[fcn]->bar0 + 
	                    mmio->fcn_cfg_array[fcn]->afu_cfg_array[afuid]->pp_mmio_offset + 
	                    ( mmio->fcn_cfg_array[fcn]->afu_cfg_array[afuid]->pp_mmio_stride * client->context ) + 
	                    addr;
	    debug_msg( "per pasid mmio to fcn/afu %d/%d : offset (0x%016lx) + ( stride (0x%016lx) * context (%d) ) + addr (0x%016lx) = cmd_pa (0x%016lx)", 
		       fcn, 
		       afuid, 
		       mmio->fcn_cfg_array[fcn]->afu_cfg_array[afuid]->pp_mmio_offset, 
		       mmio->fcn_cfg_array[fcn]->afu_cfg_array[afuid]->pp_mmio_stride, 
		       client->context, 
		       addr,
		       event->cmd_PA );
	  }
	}
	// event->addr = addr;
	event->cfg = cfg;
	event->cmd_data = data;
	event->client = client;
	event->state = OCSE_IDLE;
	event->_next = NULL;

	// debug the mmio and print the input address and the translated address
	// debug_msg("_add_event: %s: WRITE%d word=0x%05x (0x%05x) data=0x%s",
	 debug_msg("_add_event:: Access word=0x%016lx (0x%016lx) data=0x%016lx",
	 //	  mmio->afu_name, event->dw ? 64 : 32,
	 	  event->cmd_PA, addr, event->cmd_data);

	// Add to end of list
	list = &(mmio->list);
	while (*list != NULL)
		list = &((*list)->_next);
	*list = event;
	
	if (mmio->list != NULL) {
	  debug_msg( "_add_event put an event in mmio list " );
	} else {
	  debug_msg( "_add_event DID NOT put an event in mmio list " );
	}
	if (cfg)
		context = -1;
	else
		context = client->context;
	debug_mmio_add(mmio->dbg_fp, mmio->dbg_id, context, rnw, dw, addr);

	return event;
}

// create a new _add_mem_event function that will use size instead of dw.
// Add new mmio event for general memory transfer
struct mmio_event *add_kill_xlate_event(struct mmio *mmio, struct client *client,
				     uint64_t ea, uint8_t pg_size, uint8_t cmd_flag, uint16_t bdf,
				     uint32_t pasid)
{
	struct mmio_event *event;
	struct mmio_event *this_mmio;
	//struct mmio_event **list;

	debug_msg("add_kill_xlate_event: entered for 0x%016lx", ea);

	// Add new event in IDLE state
	event = (struct mmio_event *)calloc(1, sizeof(struct mmio_event));

	if (!event)
		return event;

	event->cmd_CAPPtag = mmio->CAPPtag_next++;
	event->cmd_opcode = OCSE_KILL_XLATE ;
	event->cmd_flg = cmd_flag;
	event->cmd_PA = ea;  // hopefully cmd_PA will be put on EA during send_mmio
	event->cmd_bdf = bdf;
	event->cmd_pasid = pasid;
	event->cmd_pg_size = pg_size;

	event->client = client; 
	event->state = OCSE_IDLE;
	event->_next = NULL;

	// Add to end of list.
	if (mmio->list == NULL) {
	  debug_msg("add_kill_xlate_event: mmio->list = event");
	  mmio->list = event;
	} else {
	  this_mmio = mmio->list;
	  debug_msg("add_kill_xlate_event: scanning mmio->list");
	  while (this_mmio->_next != NULL) {
	    debug_msg("add_kill_xlate_event: this_mmio = this_mmio->_next");
	    this_mmio = this_mmio->_next;
	  }
	  debug_msg("add_kill_xlate_event: this_mmio->_next = event");
	  this_mmio->_next = event;
	}
	//list = &(mmio->list);
	//while (*list != NULL)
	//	list = &((*list)->_next);
	//*list = event;

	debug_msg("add_kill_xlate_event: @0x%016lx added KILL_XLATE with CAPPtag=0x%04x for 0x%016lx", event, event->cmd_CAPPtag, event->cmd_PA);
	return event;
}

// create a new _add_mem_event function that will use size instead of dw.
// Add new mmio event for general memory transfer
static struct mmio_event *_add_mem_event(struct mmio *mmio, struct client *client,
				     uint32_t rnw, uint32_t size, int region, uint64_t addr,
				     uint8_t *data, uint32_t be_valid, uint64_t be)
{
	struct mmio_event *event;
	struct mmio_event **list;
	uint8_t fcn; 
	uint8_t afuid;
	uint16_t context;

	// Add new event in IDLE state
	event = (struct mmio_event *)calloc(1, sizeof(struct mmio_event));
	if (!event)
		return event;
	event->cfg = 0;
	event->cmd_opcode = 0; // don't get this confused with amo cmds!
	event->rnw = rnw;
	event->be_valid = be_valid;
	event->dw = 0;
	event->size = size;  // part of the new fields
	event->size_received = 0;  // part of the new fields
	event->data = data;
	event->be = be;
	if (client == NULL)  {
	  // is this case where cfg = 1, that is, we want to read config space?
	  // yes, when we do mmios to config space, we force client to null
	  event->cmd_PA = addr;
	} else {
	  // for OpenCAPI, the memory space is split into LPC, global and per pasid
	  // the region parm controls how we adjust the offset prior to adding the event
	  // technically, all of these should be adjusted by the BAR specified in the configuration... ocse assumes a BA of 0
	  //   region = 0 means we are LPC memory and offset is unadjusted
	  //   region = 1 means we adjust offset based on the global mmio offset from the configuration
	  //   region = 2 means we want to send the offset adjusted by the per pasid mmio offset, per pasid mmio stride, and client index
	  //   for now, we are assuming the client index (context) maps directly to a pasid.  
	  //        we could be more creative and relocate the pasid base and pasid length supported to 
	  //        provide more verification coverage
	  fcn = client->fcn;
	  afuid = client->afuid;

	  if (region == 0) {
	    // lpc area
	    event->cmd_PA = addr;
	  } else if (region == 1) {
	    // global mmio offset + offset
	    event->cmd_PA = mmio->fcn_cfg_array[fcn]->afu_cfg_array[afuid]->global_mmio_offset + addr;
	  } else {
	    // per pasid mmio offset + (client context * stride) + offset
	    event->cmd_PA = 
	      mmio->fcn_cfg_array[fcn]->afu_cfg_array[afuid]->pp_mmio_offset + 
	      ( mmio->fcn_cfg_array[fcn]->afu_cfg_array[afuid]->pp_mmio_stride * client->context) + 
	      addr;
	  }
	}
	event->client = client;
	event->state = OCSE_IDLE;
	event->_next = NULL;

	debug_msg("_add_mem_event: rnw=%d, access word=0x%016lx (0x%016lx)", event->rnw, event->cmd_PA, addr);
#ifdef DEBUG
	printf("_add_mem_event: data = 0x" );
	int i;
	for ( i=0; i<size; i++ ) {
	  printf( "%02x", event->data[i] );
	}
	printf( "\n" );
#endif

	// Add to end of list
	list = &(mmio->list);
	while (*list != NULL)
		list = &((*list)->_next);
	*list = event;
	if (event->cfg)
		context = -1;
	else
		context = client->context;
	debug_mmio_add(mmio->dbg_fp, mmio->dbg_id, context, rnw, size, addr);

	return event;
}

// Add AFU config space (config_rd, config_wr) access event
static struct mmio_event *_add_cfg(struct mmio *mmio, uint32_t rnw,
				    uint32_t dw, uint64_t addr, uint64_t data)
{
  debug_msg( "_add_cfg:" );
        return _add_event(mmio, NULL, rnw, dw, 0, addr, 1, data);
}

// Add AFU MMIO (non-config) access event
static struct mmio_event *_add_mmio(struct mmio *mmio, struct client *client,
				    uint32_t rnw, uint32_t dw, int global, uint64_t addr,
				    uint64_t data)
{
	return _add_event(mmio, client, rnw, dw, global, addr, 0, data);
}

// Add AFU general memory access command event
static struct mmio_event *_add_mem(struct mmio *mmio, struct client *client,
				    uint32_t rnw, uint32_t size, int region, uint64_t addr,
				   uint8_t *data, uint32_t be_valid, uint64_t be)
{
  return _add_mem_event(mmio, client, rnw, size, region, addr, data, be_valid, be);
}

static void _wait_for_done(enum ocse_state *state, pthread_mutex_t * lock)
{
	while (*state != OCSE_DONE)	/* infinite loop */
		lock_delay(lock);
}

// Read the AFU descriptor template 0 from the afu information DVSEC
// pass in the address of the afu descriptor offset register
//         the offset of the descriptor requested
// returns an mmio event with the data from the requested offset
static struct mmio_event *_read_afu_descriptor(struct mmio *mmio, uint64_t addr, uint64_t offset, pthread_mutex_t * lock)
{
  struct mmio_event *event0c;
  struct mmio_event *event10;

  #define AFU_DESC_DATA_VALID 0x80000000
  #define FUNCTION_CFG_OFFSET 0x0000000000010000; // per spec, each function has some config space 


  // step 1: write the offset of the descriptor that we want in the afu descriptor register
  debug_msg("_read_afu_descriptor: AFU descriptor offset 0x%016lx indirect read", offset);
  debug_msg("   AFU Information DVSEC write 0x%08x @ 0x%016lx", offset, addr);
  event0c = _add_cfg(mmio, 0, 0, addr, offset);
  _wait_for_done(&(event0c->state), lock);
  debug_msg("   AFU Information DVSEC write 0x%08x @ 0x%016lx complete", offset, addr);
  free(event0c);

  // step 2: read the afu descriptor offset register looking for the data valid bit to become 1
  event0c = _add_cfg(mmio, 1, 0, addr, 0L);
  _wait_for_done(&(event0c->state), lock);
  debug_msg("   AFU Information DVSEC read @ 0x%016lx = 0x%08x", addr, event0c->cmd_data);
  
  while ((event0c->cmd_data & AFU_DESC_DATA_VALID) == 0) {
    free(event0c);
    event0c = _add_cfg(mmio, 1, 0, addr, 0L);
    _wait_for_done(&(event0c->state), lock);
    debug_msg("   AFU Information DVSEC read @ 0x%016lx = 0x%08x", addr, event0c->cmd_data);
  }
  free(event0c);
  debug_msg("   AFU descriptor offset 0x%016lx indirect read ready", offset);

  // step 3: read the data from the afu descriptor data register
  event10 = _add_cfg(mmio, 1, 0, addr + 4, 0L);  // assuming the data register is adjacent to the offset register
  _wait_for_done(&(event10->state), lock);
  debug_msg("   AFU Information DVSEC afu descriptor data read 0x%08x @ 0x%016lx complete", event10->cmd_data, addr + 4);

  return event10;
}

// Read the AFU config_record, extended capabilities (if any), PASID extended capabilities,
// OpenCAPI TL extended capabilities, AFU info extended capabilites (AFU descriptor)
// and AFU control information extended capabilities and keep a copy
int read_afu_config(struct ocl *ocl, uint8_t bus, pthread_mutex_t * lock)
{
	debug_msg("read_afu_config: and WON'T BE ABLE TO SEND CMD UNTIL AFU GIVES US INITIAL CREDIT!!");
	uint8_t   afu_tlx_vc1_credits_available;
	uint8_t   afu_tlx_vc2_credits_available;
	uint8_t   cfg_tlx_credits_available;
	uint8_t   afu_tlx_vc0_credits_available;

	struct mmio *mmio;
	mmio = ocl->mmio;

	#define AFU_DESC_DATA_VALID 0x80000000

	debug_msg("read_afu_config: before read initial credits");
	while ( afu_tlx_read_initial_credits( mmio->afu_event, &afu_tlx_vc0_credits_available, &afu_tlx_vc1_credits_available,
					      &afu_tlx_vc2_credits_available, &cfg_tlx_credits_available) != TLX_SUCCESS ){
	  //infinite loop
	  sleep(1);
	} 
	info_msg("read_afu_config: afu_tlx_vc0_credits_available= %d, afu_tlx_vc1_credits_available= %d, afu_tlx_vc2_credits_available= %d, cfg_tlx_credits_available= %d",
		afu_tlx_vc0_credits_available, afu_tlx_vc1_credits_available, afu_tlx_vc2_credits_available,
		cfg_tlx_credits_available);

        uint64_t cmd_pa_bus ; // the base physical address of the bus we are working on
        uint64_t cmd_pa_fcn ; // bus + function offset
        uint64_t cmd_pa_ec ;  // bus + function offset + next_capability offset
	uint64_t next_capability_offset;
	uint64_t bar, bar0; // , bar1, bar2;
	// uint64_t actag;
	uint64_t pasid;
	uint16_t device_id, vendor_id;
	uint16_t ec_id;
	int f;
	int afu_index;
	struct mmio_event *eventa, *eventb, *eventc;

	ocl->max_clients = 0;
	ocl->max_actags = 0;

	cmd_pa_bus = (uint64_t)bus << 24; // shift the bus number to the proper location in the pa

	// allocate space for function configuration information
	mmio->fcn_cfg_array = (struct fcn_cfg **)malloc( 8 * sizeof( struct fun_cfg * ) );

	// loop through all the potential functions by incrementing the fcn portion of the physical address.
	// eventually, we might want to set up "bus" in a parm file.  For now, we assume bus = 0
	// init high water marks so we can build up base values for BARs and acTag
	// 5/24/2018 - in Power Systems, the initial bar setting is controlled by the processor side
	// it is NOT calculated based on the the memory usage profile that can be obtained by walking the
	// config space.  The initial value used in Power is 4 TB (the LPC size reserved by the OS)
	bar = 0x0000040000000000; // 4 TB

	for (f = 0; f < 8; f++ ) {
  	        // reset the high water mark for pasid
 	        pasid = 0;
	        cmd_pa_fcn = cmd_pa_bus + ( f * 0x10000 );

		// read open capi configuration space header
		debug_msg("_read_config_space_header:  pa 0x%016lx ", cmd_pa_fcn);
		eventa  = _add_cfg(mmio, 1, 0, cmd_pa_fcn, 0L); // opencapi configuration header
  		_wait_for_done( &(eventa->state), lock );
		device_id = (uint16_t)( ( eventa->cmd_data >> 16 ) & 0x0000FFFF);
		vendor_id = (uint16_t)( eventa->cmd_data & 0x0000FFFF );
		free( eventa );

       		info_msg("OpenCAPI Configuration header for function %d", f );
       		info_msg("    0x%04x:0x%04x device & vendor", device_id, vendor_id );

		// skip for now, but eventually read offset + 0x04 and possibly offset + 0x34 to get capabilites info

		// if device and vendor id are not 0 or FFFF, we have a valid function config space
		if ( ( ( device_id != 0 ) && ( device_id != 0xffff ) ) && 
		     ( ( vendor_id != 0 ) && ( vendor_id != 0xffff ) ) ) {
         	      // allocate the fcn_cfg structure and store the pointer at mmio->fcn_cfg_array_p[f]
		      mmio->fcn_cfg_array[f] = (struct fcn_cfg *)malloc( sizeof( struct fcn_cfg ) );
		      mmio->fcn_cfg_array[f]->device_id = device_id;
		      mmio->fcn_cfg_array[f]->vendor_id = vendor_id;
					
		      info_msg("    function %d bar0 = 0x%016lx", f, mmio->fcn_cfg_array[f]->bar0 );

		      next_capability_offset = 0x100;
		      while ( next_capability_offset != 0 ) {
			  // Read extended capabilities - offset + 0x100  [31:20] next ec offset, [7:0] this ec ID
			  cmd_pa_ec = cmd_pa_fcn + next_capability_offset;
			  eventa  = _add_cfg( mmio, 1, 0, cmd_pa_ec, 0L ); // extended capabilities
			  _wait_for_done( &(eventa->state), lock );
			
			  ec_id = (uint16_t)( eventa->cmd_data & 0x0000FFFF );
			  
			  // based on the ec_id that we read, do some additional processing
			  switch (ec_id) 	{
				case 0x0003: 
				     info_msg("    skipping a DSN extended capability 0x%04x at offset 0x%04x - skipping", ec_id, next_capability_offset );
				     // skip for now
				     break;
				case 0x001b: 
				     info_msg("    Found a PASID extended capability 0x%04x at offset 0x%04x", ec_id, next_capability_offset );
				     eventb = _add_cfg( mmio, 1, 0, cmd_pa_ec + 0x04, 0L ); 
				     _wait_for_done(&(eventb->state), lock);
				     mmio->fcn_cfg_array[f]->max_pasid_width = eventb->cmd_data >> 8;
				     ocl->max_clients = ocl->max_clients + ( 1 << mmio->fcn_cfg_array[f]->max_pasid_width );
				     // this functions max clients is 1 << max_pasid_width
				     info_msg("    Max PASID width is 0x%08x ", mmio->fcn_cfg_array[f]->max_pasid_width );
				     free( eventb );
				     break;
				case 0x0023: 
				     info_msg("    Found a OpenCAPI DVSEC 0x%04x at offset 0x%04x", ec_id, next_capability_offset );
				     // we need to read the dvsec id to learn what to do next
				     eventb = _add_cfg( mmio, 1, 0, cmd_pa_ec + 0x08, 0L ); 
				     _wait_for_done( &(eventb->state), lock );
				     switch ( eventb->cmd_data & 0x0000FFFF ) {
				          case 0xF000: 
					       info_msg("    Found OpenCAPI TL DVSEC ");
					       // make these subroutines ???
					       eventc = _add_cfg( mmio, 1, 0, cmd_pa_ec + 0x0c, 0L ); 
					       _wait_for_done( &(eventc->state), lock );
					       mmio->fcn_cfg_array[f]->tl_major_version_capability = ( eventc->cmd_data & 0xff000000 ) >> 24;
					       mmio->fcn_cfg_array[f]->tl_minor_version_capability = ( eventc->cmd_data & 0x00ff0000 ) >> 16;
					       free( eventc );

					       eventc = _add_cfg( mmio, 1, 0, cmd_pa_ec + 0x24, 0L ); 
					       _wait_for_done( &(eventc->state), lock );
					       mmio->fcn_cfg_array[f]->tl_xmit_template_cfg = eventc->cmd_data;
					       free( eventc );

					       eventc = _add_cfg( mmio, 1, 0, cmd_pa_ec + 0x6c, 0L ); 
					       _wait_for_done( &(eventc->state), lock );
					       mmio->fcn_cfg_array[f]->tl_xmit_rate_per_template_cfg = eventc->cmd_data;
					       free( eventc );
					       info_msg( "    major = 0x%02x, minor = 0x%02x, xmit_template_cfg = 0x%08x, xmit_rate_per_template_cfg = 0x%08x", 
							 mmio->fcn_cfg_array[f]->tl_major_version_capability, 
							 mmio->fcn_cfg_array[f]->tl_minor_version_capability, 
							 mmio->fcn_cfg_array[f]->tl_xmit_template_cfg, 
							 mmio->fcn_cfg_array[f]->tl_xmit_rate_per_template_cfg );
					       break;
				          case 0xF001: 
					       info_msg( "    Found FUNCTION DVSEC" );
					       mmio->fcn_cfg_array[f]->afu_present = ( eventb->cmd_data >> 24 ) & 0x80;
					       mmio->fcn_cfg_array[f]->max_afu_index = ( eventb->cmd_data >> 24 ) & 0x3F;
					       // save the current pa so we can use it later to
					       //    write function_actag_base now as we are tracking a high water mark
					       //    write function_actag_length_enabled has to be set after we know all the afu_actag_length_enabled's we have set.
					       mmio->fcn_cfg_array[f]->function_dvsec_pa = cmd_pa_ec;
					       // do this only if there afu_present is not 0
					       if ( mmio->fcn_cfg_array[f]->afu_present != 0 ) {
						     mmio->fcn_cfg_array[f]->afu_cfg_array = 
						       (struct afu_cfg **)malloc( sizeof( struct afu_cfg * ) * ( mmio->fcn_cfg_array[f]->max_afu_index + 1 ) );

						     // one or more afu's are present, discover and set the BAR's
						     // write all 1's to the bar lo/hi
						     eventc  = _add_cfg(mmio, 0, 0, cmd_pa_fcn + 0x10, 0xFFFFFFFF );
						     _wait_for_done( &(eventc->state), lock );
						     free( eventc );
						     eventc  = _add_cfg(mmio, 0, 0, cmd_pa_fcn + 0x14, 0xFFFFFFFF );
						     _wait_for_done( &(eventc->state), lock );
						     free( eventc );
						     
						     // read bar lo/hi
						     eventc  = _add_cfg(mmio, 1, 0, cmd_pa_fcn + 0x10, 0x00 );
						     _wait_for_done( &(eventc->state), lock );
						     // the low order 4 bits of cmd_data have some reserved data not related to the window, mask them off
						     bar0 = eventc->cmd_data & 0xFFFFFFF0;
						     free( eventc );
						     eventc  = _add_cfg(mmio, 1, 0, cmd_pa_fcn + 0x14, 0x00 );
						     _wait_for_done( &(eventc->state), lock );
						     bar0 = bar0 | ( eventc->cmd_data << 32 );
						     free( eventc );
		      
						     // bar0 represents the "window" of address bits that are available
						     // bar represents the current high water mark (ie, the next available base address)
						     // write bar to mmio->fcn_cfg_array[f]->bar0 
						     //    OpenCAPI Configuration Header 0x10 = bar0 low
						     //    OpenCAPI Configuration Header 0x14 = bar0 high
						     mmio->fcn_cfg_array[f]->bar0 = bar;
						     eventc  = _add_cfg(mmio, 0, 0, cmd_pa_fcn + 0x10, mmio->fcn_cfg_array[f]->bar0 & 0xFFFFFFFF );
						     _wait_for_done( &(eventc->state), lock );
						     free( eventc );
						     eventc  = _add_cfg(mmio, 0, 0, cmd_pa_fcn + 0x14, mmio->fcn_cfg_array[f]->bar0 >> 32 );
						     _wait_for_done( &(eventc->state), lock );
						     free( eventc );
						     
						     // add the size of the current window to bar
						     bar = bar + ( ~( bar0 ) + 1 );
		      
						     // repeat for bar1 and bar2 eventually 
					       
						     mmio->fcn_cfg_array[f]->function_actag_base = ocl->max_actags;
								
						     // one or more afu's are present, so set the memory space bit in the configuration space header
						     eventc  = _add_cfg(mmio, 0, 0, cmd_pa_fcn + 0x04, 0x00000002 );
						     _wait_for_done( &(eventc->state), lock );
						     free( eventc );
						     info_msg( "    Enabled memory space for function %d", f );	
					       }
					       info_msg( "    function %d afu present = 0x%02x, max afu index = 0x%02x", 
							 f,
							 mmio->fcn_cfg_array[f]->afu_present, 
							 mmio->fcn_cfg_array[f]->max_afu_index );
					       break;
				         case 0xF003: 
					      info_msg("    Found AFU INFO DVSEC ");
					      // save this pa for later when we know an afu_index that we want info for
					      mmio->fcn_cfg_array[f]->afu_information_dvsec_pa = cmd_pa_ec;
					      break;
				         case 0xF004: 
					      info_msg("    Found AFU CTL DVSEC ");
					      // if we encounter this structure and afu_present - 0, skip all this as we aren't expecting to use any of it
					      if (mmio->fcn_cfg_array[f]->afu_present == 0) break;

					      // we should have an array of pointers to afu cfg structures
					      // allocat a structure and save the pointer in mmio->fcn_cfg_array[f]->afu_cfg_p_array[afu_control_index]
					      // this index is somewhat temporary as we only need it here to access the afu descriptor
					      // and index intor the afu cfg array.
					      afu_index = ( eventb->cmd_data >> 16 ) & 0x003F;
					      // alloc an afu_cfg and store the pointer in afu_cfg_p_array[afu_index]
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index] = (struct afu_cfg *)malloc( sizeof( struct afu_cfg ) );

					      // read 0x10
					      eventc = _add_cfg( mmio, 1, 0, cmd_pa_ec + 0x10, 0L ); 
					      _wait_for_done( &(eventc->state), lock );
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pasid_len_supported = ( eventc->cmd_data ) & 0x1F;
					      free( eventc );
					      // read 0x18
					      eventc = _add_cfg( mmio, 1, 0, cmd_pa_ec + 0x18, 0L ); 
					      _wait_for_done( &(eventc->state), lock );
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->actag_length_supported = ( eventc->cmd_data ) & 0xFFF;
					      free( eventc );
					      // read afu descriptor data for this afu
					      //     write afu_index to afu_information_ec_pa.afu_info_index
					      eventc = _add_cfg( mmio, 0, 0, mmio->fcn_cfg_array[f]->afu_information_dvsec_pa + 0x08, afu_index << 16 ); 
					      _wait_for_done( &(eventc->state), lock );
					      free( eventc );

					      //     read name space
					      int i, j;
					      uint64_t name_offset = 0x04;
					      uint64_t name_stride = 0x04;
					      for (i = 0; i < 6; i++ ) {
						eventc = _read_afu_descriptor( mmio, mmio->fcn_cfg_array[f]->afu_information_dvsec_pa + 0x0c, name_offset, lock );
						for ( j = 0; j < name_stride; j++ ) {
						  // suppress '.' in namespace - replace with '\0'
						  if ( ( (uint8_t *)&eventc->cmd_data )[j] =='.' ) mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->namespace[( i*name_stride ) + j] = '\0';
						  else mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->namespace[( i*name_stride ) + j] = ( (uint8_t *)&eventc->cmd_data )[j];
						}
						name_offset = name_offset + name_stride;
						free( eventc );
					      }
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->namespace[24] = '\0'; // make sure name space is null terminated
					      info_msg("name space is %s ", mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->namespace);

					      //     read major/minor version
					      eventc = _read_afu_descriptor( mmio, mmio->fcn_cfg_array[f]->afu_information_dvsec_pa + 0x0c, 0x1c, lock );
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->afu_version_major = ( eventc->cmd_data >> 24 ) & 0xFF;
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->afu_version_minor = ( eventc->cmd_data >> 16 ) & 0xFF;
					      free( eventc );

					      //     read global mmio bar/offset lo/offset hi
					      eventc = _read_afu_descriptor( mmio, mmio->fcn_cfg_array[f]->afu_information_dvsec_pa + 0x0c, 0x20, lock );
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->global_mmio_bar = ( eventc->cmd_data ) & 0x07;
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->global_mmio_offset = ( eventc->cmd_data ) & 0xFFFF0000;
					      free( eventc );
					      eventc = _read_afu_descriptor( mmio, mmio->fcn_cfg_array[f]->afu_information_dvsec_pa + 0x0c, 0x24, lock );
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->global_mmio_offset = 
						mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->global_mmio_offset | ( eventc->cmd_data << 32 );
					      free( eventc );
					      eventc = _read_afu_descriptor( mmio, mmio->fcn_cfg_array[f]->afu_information_dvsec_pa + 0x0c, 0x28, lock );
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->global_mmio_size = eventc->cmd_data ;
					      free( eventc );
					      info_msg( "    afu %d global mmio bar = %d, global mmio offset = 0x%016lx, global mmio size = 0x%016lx", 
							afu_index,
							mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->global_mmio_bar, 
							mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->global_mmio_offset, 
							mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->global_mmio_size );

					      //     read per pasid mmio bar/offset hi/offset lo
					      eventc = _read_afu_descriptor( mmio, mmio->fcn_cfg_array[f]->afu_information_dvsec_pa + 0x0c, 0x30, lock );
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pp_mmio_bar = ( eventc->cmd_data ) & 0x07;
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pp_mmio_offset = ( eventc->cmd_data ) & 0xFFFF0000;
					      free( eventc );
					      eventc = _read_afu_descriptor( mmio, mmio->fcn_cfg_array[f]->afu_information_dvsec_pa + 0x0c, 0x34, lock );
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pp_mmio_offset = 
						mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pp_mmio_offset | ( eventc->cmd_data << 32 );
					      free( eventc );
					      eventc = _read_afu_descriptor( mmio, mmio->fcn_cfg_array[f]->afu_information_dvsec_pa + 0x0c, 0x38, lock );
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pp_mmio_stride = eventc->cmd_data & 0xFFFF0000 ;
					      free( eventc );
					      info_msg( "    afu %d per pasid mmio bar = %d, per pasid mmio offset = 0x%016lx, per pasid mmio stride = 0x%016lx", 
							afu_index,
							mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pp_mmio_bar, 
							mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pp_mmio_offset, 
							mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pp_mmio_stride );

					      //     read mem base address/size
					      eventc = _read_afu_descriptor( mmio, mmio->fcn_cfg_array[f]->afu_information_dvsec_pa + 0x0c, 0x3c, lock );
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->mem_size = eventc->cmd_data & 0xFF ;
					      free( eventc );
					      eventc = _read_afu_descriptor( mmio, mmio->fcn_cfg_array[f]->afu_information_dvsec_pa + 0x0c, 0x40, lock );
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->mem_base_address = eventc->cmd_data;
					      free( eventc );
					      eventc = _read_afu_descriptor( mmio, mmio->fcn_cfg_array[f]->afu_information_dvsec_pa + 0x0c, 0x44, lock );
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->mem_base_address = 
						mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->mem_base_address | ( eventc->cmd_data << 32 );
					      free( eventc );

					      //     write pasid_length_enabled (same as _supported)
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pasid_len_enabled = 
						mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pasid_len_supported;
					      eventc = _add_cfg( mmio, 0, 0, cmd_pa_ec + 0x10, mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pasid_len_enabled << 8 ); 
					      _wait_for_done( &(eventc->state), lock );
					      free( eventc );

					      //   write pasid base...  
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pasid_base = pasid;
					      eventc = _add_cfg( mmio, 0, 0, cmd_pa_ec + 0x14, mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pasid_base ); 
					      _wait_for_done( &(eventc->state), lock );
					      free( eventc );

					      info_msg( "    afu %d pasid base = 0x%05x, pasid length supported = 0x%02x, pasid length enabled = 0x%02x", 
							afu_index,
							mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pasid_base, 
							mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pasid_len_supported, 
							mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pasid_len_enabled );

					      // convert length enabled from a power of 2 to a number
					      pasid = pasid + pow( 2, (uint64_t)mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->pasid_len_enabled );
					      
					      //   write actag_length_enabled (same as _supported) and update function_actag_length_enabled
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->actag_length_enabled = 
						mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->actag_length_supported;
					      mmio->fcn_cfg_array[f]->function_actag_length_enabled = 
						mmio->fcn_cfg_array[f]->function_actag_length_enabled + mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->actag_length_enabled;
					      eventc = _add_cfg( mmio, 0, 0, cmd_pa_ec + 0x18, mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->actag_length_enabled << 16 ); 
					      _wait_for_done( &(eventc->state), lock );
					      free( eventc );
					      
					      // actag base...  
					      mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->actag_base = ocl->max_actags;
					      eventc = _add_cfg( mmio, 0, 0, cmd_pa_ec + 0x1c, mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->actag_base ); 
					      _wait_for_done( &(eventc->state), lock );
					      free( eventc );

					      info_msg( "    afu %d actag base = 0x%03x, actag length supported = 0x%03x, actag length enabled = 0x%03x", 
							afu_index,
							mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->actag_base, 
							mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->actag_length_supported, 
							mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->actag_length_enabled );

					      // add the actag length enabled to the actag high water mark
					      ocl->max_actags = ocl->max_actags + mmio->fcn_cfg_array[f]->afu_cfg_array[afu_index]->actag_length_enabled;

					      //   rwrite afu_control_dvsec(0x0c) enable afu - can I really do this here?
					      eventc = _add_cfg(mmio, 0, 0, cmd_pa_ec + 0x0c, 0x01000000);
					      _wait_for_done(&(eventc->state), lock);
					      info_msg("    afu %d enabled", afu_index );

					      break;
				         case 0xF0F0: 
					      warn_msg("Found VENDOR SPECIFIC DVSEC 0x%08x - skipping ", eventb->cmd_data);
					      break;
				         default:
					      warn_msg ("FOUND something UNEXPECTED in DVSEC 0x%08x - skipping ", eventb->cmd_data);
					      break;
				     } // end of switch dvsec_id	

				     free(eventb);
				     break;
				default:   
				     warn_msg ("FOUND something UNEXPECTED in EC 0x%016lx - skipping ", ec_id);
				     break;

			  } // end of switch ec_id

			  // advance to the next capability
			  next_capability_offset = (uint16_t)((eventa->cmd_data & 0xFFF00000) >> 20);
			  free(eventa);

		      } // end of read extended capability loop

		      // before leave this function to attempt the next one...
		      // write function_actag_base to function dvsec
		      // write function_actag_length_enabled to function dvsec - actag is tracking the device actag high water mark
		      eventa  = _add_cfg( mmio, 0, 0, mmio->fcn_cfg_array[f]->function_dvsec_pa + 0x0c, 
					  ( ( (uint32_t)mmio->fcn_cfg_array[f]->function_actag_base << 16 ) | 
					    (uint32_t)mmio->fcn_cfg_array[f]->function_actag_length_enabled ) );
		      _wait_for_done( &(eventa->state), lock );
		      free( eventa );

		      info_msg( "    function %d actag base = 0x%04x, actag length enabled = 0x%04x ", 
				f,
				mmio->fcn_cfg_array[f]->function_actag_base, 
				mmio->fcn_cfg_array[f]->function_actag_length_enabled );


		} // end of if valid config space header
  	
	} // end of read function csh loop
	
	return 0;
}

// modify to check command and use size, dl dp and stuff...
// Send pending MMIO event to AFU; use config_read or config_write for descriptor
// for MMIO use cmd_pr_rd_mem or cmd_pr_wr_mem
// For TLX_CMD_KILL_XLATE make sure to use vc2 NOT vc1
void send_mmio(struct mmio *mmio)
{
	struct mmio_event *event;
	char type[7];
	//unsigned char ddata[17];
	unsigned char null_buff[256] = {0};
	unsigned char tdata_bus[256];
	uint8_t *dptr = tdata_bus;
	char data[17];
	uint16_t  cmd_byte_cnt;
	uint64_t offset;

#ifdef DEBUG
	int i;
#endif
	
	// debug_msg( "send_mmio: " );

	event = mmio->list;

	// Check for valid event
	if ( event == NULL ) 
		return;
	// debug_msg( "send_mmio: there is a valid command" );

	if ( event->state != OCSE_IDLE ) // the mmio has already been sent
		return;
	debug_msg( "send_mmio: valid command is ready to send" );

	event->ack = OCSE_MMIO_ACK;
	if (event->cfg) {
	        // debug_msg( "ocse:send_mmio:mmio to config space" );
		sprintf(type, "CFG");
		// Attempt to send config_rd or config_wr to AFU
		if (event->rnw) { //for config reads, no data to send
			if ( tlx_afu_send_cfg_cmd_and_data(mmio->afu_event,
							   TLX_CMD_CONFIG_READ, 0xdead, 2, 0, event->cmd_PA,
							   0,0) == TLX_SUCCESS) {
				debug_msg("%s:%s READ%d word=0x%05x", mmio->afu_name, type,
			  	 	event->dw ? 64 : 32, event->cmd_PA);
				debug_mmio_send(mmio->dbg_fp, mmio->dbg_id, event->cfg,
					event->rnw, event->dw, event->cmd_PA);
				event->state = OCSE_PENDING;
			}
		} else { //for config writes and we ALWAYS send 32 bits of data
			// restricted by spec to pL of 1, 2, or 4 bytes HOWEVER
			// We now have to offset the data into a 32B buffer and send it
			memcpy(tdata_bus, null_buff, 32); //not sure if we always have to do this, but better safe than...
			offset = event->cmd_PA & 0x0000000000000003 ;
			memcpy(dptr + offset, &(event->cmd_data), 4);
			if ( tlx_afu_send_cfg_cmd_and_data( mmio->afu_event,
							    TLX_CMD_CONFIG_WRITE, 0xbeef, 2, 0, event->cmd_PA,
							    0,dptr) == TLX_SUCCESS) {
			                sprintf(data, "%08" PRIx32, (uint32_t) event->cmd_data);
					debug_msg("%s:%s WRITE%d word=0x%05x data=0x%s offset=0x%x",
						  mmio->afu_name, type,  32,
						  event->cmd_PA, data, offset);
					debug_mmio_send(mmio->dbg_fp, mmio->dbg_id, event->cfg,
							event->rnw, event->dw, event->cmd_PA);
					event->state = OCSE_PENDING;
			}
		}
		return;
       	}  

	// if not a CONFIG, then must be memory access MMIO rd/wr or a kill_xlate or a force_evict
	if ( event->size == 0 ) {
	      // we have the old mmio style
	      debug_msg( "send_mmio: mmio to mmio space" );
	      sprintf(type, "MMIO");
	      
	      event->cmd_dL = 0;
	      event->cmd_dP = 0;

	      // calculate event->pL from event->dw
	      // calculate cmd_byte_cnt from event->dw
	      if (event->dw == 1) {
		     // pl = 3 ::= 8 bytes
		     event->cmd_pL = 3;
		     cmd_byte_cnt = 8;
	      } else {
		     // pl = 2 ::= 4 bytes
		     event->cmd_pL = 2;
		     cmd_byte_cnt = 4;
	      }

	      // fix the data pointer for the write command later
	      // ???
	      event->data = (uint8_t *)&(event->cmd_data);

	} else {
	      // we have the new general memory style
	      debug_msg( "send_mmio: mmio to CMD/LPC space" );
	      sprintf(type, "MEM");
	      event->ack = OCSE_LPC_ACK;
	      
	      // calculate event->pL, dL, and dP from event->dw
	      // calculate cmd_byte_cnt from event->size
	      cmd_byte_cnt = event->size;
	      event->cmd_pL = 0;
	      event->cmd_dL = 0;
	      event->cmd_dP = 0;
	      switch (event->size) {
	      case 1:
		    break;
	      case 2:
		    event->cmd_pL = 1;
		    break;
	      case 4:
		    if (( event->cmd_flg >= 8 ) & ( event->cmd_flg <= 10 )) {
		      event->cmd_pL = 0x6;
		    } else {
		      event->cmd_pL = 0x2;
		    }
		    break;
	      case 8:
		    if (( event->cmd_flg >= 8 ) & ( event->cmd_flg <= 10 )) {
		      event->cmd_pL = 0x7;
		    } else {
		      event->cmd_pL = 0x3;
		    }
		    break;
	      case 16:
		    event->cmd_pL = 4;
		    break;
	      case 32:
		    event->cmd_pL = 5;
		    break;
	      case 64:
		    event->cmd_dL= 1;
		    break;
	      case 128:
		    event->cmd_dL= 2;
		    break;
	      case 256:
		    event->cmd_dL= 3;
		    break;
	      default:
		    warn_msg( "send_mmio: Invalid size given %d", event->size );
	      }
		      
	}
		
	// need to be a little more clever here.  an amo_rw comes over as a read, but also has to send data
	// we should perhaps do this section based on event->opcode (which is the OCSE cmd form)
	// amo needs special handling, we can tell these from cmd_opcode
	// but other lpc commands did not send along the cmd_opcode.  
	
	switch ( event->cmd_opcode ) {
	case OCSE_KILL_XLATE:
	      if ( tlx_afu_send_cmd_vc2( mmio->afu_event,
					 TLX_CMD_KILL_XLATE, 
					 event->cmd_CAPPtag, 
					 event->cmd_pg_size, 
					 event->cmd_PA, 
					 event->cmd_flg, 
					 event->cmd_pasid, 
					 event->cmd_bdf ) == TLX_SUCCESS ) {
  		    debug_msg("%s:%s XLATE_KILL %d word=0x%05x", mmio->afu_name, type, event->dw ? 64 : 32, event->cmd_PA);
		    debug_mmio_send(mmio->dbg_fp, mmio->dbg_id, event->cfg, event->rnw, event->dw,event->cmd_PA );
		    event->state = OCSE_PENDING;  // waiting for a kill_xlate_done *command* from the afu. 
	      }
	      break;
	case OCSE_AFU_AMO_RD:
	      // we know size < 64
	      debug_msg("%s:%s AMO_RD %d word=0x%05x", mmio->afu_name, type, event->dw ? 64 : 32, event->cmd_PA);
	      if ( tlx_afu_send_cmd_vc1( mmio->afu_event,
					 TLX_CMD_AMO_RD, 
					 0, 0xcafe, event->cmd_PA, 0, 0, 0, 
					 event->cmd_pL, 0, 0, 0, event->cmd_flg, 0 ) == TLX_SUCCESS ) {
  		    debug_msg("%s:%s AMO_RD %d word=0x%05x", mmio->afu_name, type, event->dw ? 64 : 32, event->cmd_PA);
		    debug_mmio_send(mmio->dbg_fp, mmio->dbg_id, event->cfg, event->rnw, event->dw,event->cmd_PA );
		    event->state = OCSE_PENDING;
	      }
	      break;
	case OCSE_AFU_AMO_WR:
	      // we know size < 64
	      debug_msg("%s:%s AMO_WR %d word=0x%05x", mmio->afu_name, type, event->dw ? 64 : 32, event->cmd_PA);
	      offset = event->cmd_PA & 0x000000000000003F ;  // this works for addresses >= 64 too */
	      memcpy(tdata_bus, null_buff, 64); // clear tdata_bus
	      memcpy( dptr+offset, event->data, cmd_byte_cnt);  // copy the data to the proper offset in tdata buffer */
#ifdef DEBUG
	      printf("send_mmio: data = 0x" );
	      for ( i=0; i<cmd_byte_cnt; i++ ) {
		printf( "%02x", tdata_bus[i] );
	      }
	      printf( "\n" );
#endif
	      if (tlx_afu_send_cmd_vc1_and_dcp1( mmio->afu_event,
						 TLX_CMD_AMO_W, 0,
						 0xbead, 
						 event->cmd_PA,
						 event->cmd_dL,0,0, 
						 event->cmd_pL, 
						 0, 0,0,event->cmd_flg,0,
						 0, // always good data for now
						 tdata_bus ) == TLX_SUCCESS) {
		    event->state = OCSE_PENDING; //OCSE_RD_RQ_PENDING;
	      }
	      break;
	case OCSE_AFU_AMO_RW:
	      // we know size < 64
	      debug_msg("%s:%s AMO_RW %d word=0x%05x", mmio->afu_name, type, event->dw ? 64 : 32, event->cmd_PA);
	      offset = event->cmd_PA & 0x000000000000003F ;  // this works for addresses >= 64 too */
	      memcpy( tdata_bus, null_buff, 64); // clear tdata_bus
	      memcpy( dptr + offset, event->data, cmd_byte_cnt);  // copy the data to the proper offset in tdata buffer */
	      // need to add the dataw to the proper offset into the 64 byte tdata_bus...
	      if (event->cmd_flg > 7) {
	      offset = event->cmd_PA & 0x000000000000000F ;
	      offset = offset + 8 ; // add 8
	      offset = offset & 0x000000000000000F ; // clear the carry if any
	      offset = ( event->cmd_PA & 0x0000000000000030 ) | offset; 
	      memcpy( dptr + offset, event->dataw, cmd_byte_cnt);  // copy the dataw to the proper offset in tdata buffer */
#ifdef DEBUG
	      printf("send_mmio: data = 0x" );
	      for ( i=0; i<cmd_byte_cnt; i++ ) {
		printf( "%02x", tdata_bus[i] );
	      }
	      printf( "\n" );
#endif
	      }
	      if (tlx_afu_send_cmd_vc1_and_dcp1( mmio->afu_event,
						 TLX_CMD_AMO_RW, 0,
						 0xbead, 
						 event->cmd_PA,
						 event->cmd_dL, 0, 0, 
						 event->cmd_pL, 
						 0, 0, 0, event->cmd_flg, 0,
						 0, // always good data for now
						 tdata_bus ) == TLX_SUCCESS) {
		    event->state = OCSE_PENDING; //OCSE_RD_RQ_PENDING;
	      }
	      break;
	case OCSE_FORCE_EVICT:
	      // This is a posted cmd that goes to AFU over VC0, which is normally used for responses
	     if ( tlx_afu_send_resp_cmd_vc0( mmio->afu_event,
					     TLX_CMD_FORCE_EVICT, 0, 0, 0, event->cmd_dL, event->cmd_host_tag, 0, 0, 
					     0, 0, 0, 0, event->cmd_CAPPtag ) == TLX_SUCCESS ) {
	             debug_msg("%s:%s send FORCE_EVICT", mmio->afu_name, type);
		     debug_mmio_send(mmio->dbg_fp, mmio->dbg_id, event->cfg, event->rnw, event->dw,event->cmd_PA );
		     event->state = OCSE_PENDING; // waiting for castout[.push]
		     // mmio->list = mmio->list->_next; // TODO ASK Lance if this is SUPPOSED to be done :)
	      }
	      break;
	case OCSE_DISABLE_CACHE:
	case OCSE_ENABLE_CACHE:
	case OCSE_DISABLE_ATC: 
	case OCSE_ENABLE_ATC: 
	      // These cmds go to AFU over VC2, not VC1 like other mmios
	      if ( tlx_afu_send_cmd_vc2( mmio->afu_event,
					 event->cmd_opcode,  //valid 
					 event->cmd_CAPPtag, //valid
					 event->cmd_pg_size, 
					 event->cmd_PA, 
					 event->cmd_flg, //valid
					 event->cmd_pasid, //valid, as is bdf 
					 event->cmd_bdf ) == TLX_SUCCESS ) {
  		    debug_msg("%s:%s ENABLE/DISABLE ATC/CACHE cmd_opcode=0x%x", mmio->afu_name, type, event->cmd_opcode);
		    debug_mmio_send(mmio->dbg_fp, mmio->dbg_id, event->cfg, event->rnw, event->dw,event->cmd_PA );
		    event->state = OCSE_PENDING;  // waiting for a cache mgmt command response back from the afu. 
	      }
	      break;
	default:
	      // use rnw and size (or cmd_byte_cnt) to sort out the rest
	      if (event->rnw) {
		    debug_msg("%s:%s READ %d word=0x%05x", mmio->afu_name, type, event->dw ? 64 : 32, event->cmd_PA);
		    if (cmd_byte_cnt < 64) { // partial
		          //TODO update event struct with new cmd fields if needed
		          if ( tlx_afu_send_cmd_vc1( mmio->afu_event,
						     TLX_CMD_PR_RD_MEM, 
						     0, 0xcafe, event->cmd_PA, 0, 0, 0, 
						     event->cmd_pL, 0, 0, 0, event->cmd_flg, 0 ) == TLX_SUCCESS ) {
			        debug_msg("%s:%s READ size=%d word=0x%05x", mmio->afu_name, type, event->dw ? 64 : 32, event->cmd_PA);
				debug_mmio_send(mmio->dbg_fp, mmio->dbg_id, event->cfg, event->rnw, event->dw,event->cmd_PA );
				event->state = OCSE_PENDING;
			  }
		    } else { // full
		          //TODO update event struct with new cmd fields if needed
		          if ( tlx_afu_send_cmd_vc1( mmio->afu_event,
						     TLX_CMD_RD_MEM, 
						     0, 0xefac, event->cmd_PA, event->cmd_dL, 0, 0, 
						     event->cmd_pL, 0, 0, 0, event->cmd_flg, 0 ) == TLX_SUCCESS ) {
			        debug_msg("%s:%s READ size=%d offset=0x%05x", mmio->afu_name, type, cmd_byte_cnt, event->cmd_PA);
				debug_mmio_send(mmio->dbg_fp, mmio->dbg_id, event->cfg, event->rnw, event->dw, event->cmd_PA);
				event->state = OCSE_PENDING;
			  }
		    }
		    debug_msg("send_mmio: sent read command, now wait for resp from AFU \n"); 
	      } else {
		    debug_msg("%s:%s WRITE %d word=0x%05x", mmio->afu_name, type, cmd_byte_cnt, event->cmd_PA);
		    if (cmd_byte_cnt < 64) { 
		          // partial
		          debug_msg("%s:%s partial WRITE %d word=0x%05x", mmio->afu_name, type, event->dw ? 64 : 32, event->cmd_PA);
		          offset = event->cmd_PA & 0x000000000000003F ;  // this works for addresses >= 64 too */
			  memcpy(tdata_bus, null_buff, 64); // clear tdata_bus
			  memcpy( dptr+offset, event->data, cmd_byte_cnt);  // copy the data to the proper offset in tdata buffer */
#ifdef DEBUG
			  printf("send_mmio: data = 0x" );
			  for ( i=0; i<cmd_byte_cnt; i++ ) {
			    printf( "%02x", tdata_bus[i] );
			  }
			  printf( "\n" );
#endif
			  if (tlx_afu_send_cmd_vc1_and_dcp1( mmio->afu_event,
							     TLX_CMD_PR_WR_MEM, 0,
							     0xbead, 
							     event->cmd_PA,
							     event->cmd_dL,0,0, 
							     event->cmd_pL, 
							     0, 0,0,event->cmd_flg,0,
							     0, // always good data for now
							     tdata_bus ) == TLX_SUCCESS) {
			        event->state = OCSE_PENDING; //OCSE_RD_RQ_PENDING;
			  }
		    } else { 
		          // full
		          memcpy( tdata_bus, event->data, cmd_byte_cnt);  // copy the data to the proper offset in tdata buffer */
#ifdef DEBUG
			  printf("send_mmio: data = 0x" );
			  for ( i=0; i<cmd_byte_cnt; i++ ) {
			    printf( "%02x", tdata_bus[i] );
			  }
			  printf( "\n" );
#endif
			  if (event->be_valid == 0){
			        debug_msg("%s:%s full WRITE %d word=0x%05x", mmio->afu_name, type, event->dw ? 64 : 32, event->cmd_PA);
			        if (tlx_afu_send_cmd_vc1_and_dcp1( mmio->afu_event,
								   TLX_CMD_WRITE_MEM, 0, // opcode and afutag
								   0xdaeb,            // capp tag
								   event->cmd_PA,
								   event->cmd_dL, 0, 0,    // dL, dp, be
								   event->cmd_pL,     // pL
								   0, 0, 0, event->cmd_flg, 0,     // endian, co , os, cmdflag, mad
								   0, // always good data for now
								   tdata_bus ) == TLX_SUCCESS) {
				  event->state = OCSE_PENDING; //OCSE_RD_RQ_PENDING;
				  debug_msg("send_mmio: sent write command, now wait for resp from AFU \n"); 
				}
			  } else {
			        debug_msg("%s:%s full WRITE BE %d word=0x%05x", mmio->afu_name, type, event->dw ? 64 : 32, event->cmd_PA);
   			        if (tlx_afu_send_cmd_vc1_and_dcp1( mmio->afu_event,
								   TLX_CMD_WRITE_MEM_BE, 0,
								   0xbebe, 
								   event->cmd_PA,
								   event->cmd_dL,0,
								   event->be, 
								   event->cmd_pL, 
								   0, 0, 0, event->cmd_flg, 0, 
								   0, // always good data for now
								   tdata_bus ) == TLX_SUCCESS) {
				  event->state = OCSE_PENDING; //OCSE_RD_RQ_PENDING;
				  debug_msg("send_mmio: sent write be command, now wait for resp from AFU \n"); 
				}
			  }
		    }
	      }
	      break;
	}
	
	
	return;
}

// Handle ap response data beats coming from the afu
// this will include responses to mmio requests, amo cmds and lpc memory requests
void handle_ap_resp_data(struct mmio *mmio)
{
	int rc;
	uint8_t resp_data_is_valid;
	uint8_t rdata_bad;
	unsigned char   rdata_bus[64];
	int offset, length;

#ifdef DEBUG
	int i;
#endif	  

	rc = 1;

	// handle mmio, and lpc response data 
	// we are expecting 1 to 4 beats of data depending on the value of dL or dw
	// we can use size to define how many bytes we expect
	// each read is 64 bytes of data, so we can track the length we have so far
	// notes:
	//   if size < 64, the interesting data is at an offset in rdata_bus

	// debug_msg( "handle_ap_resp_data: event cfg = %d, rnw = %d, current event state = %d", 
	// 	   mmio->list->cfg, 
	//	   mmio->list->rnw, 
	//	   mmio->list->state );

	if ( mmio->list->cfg ) {
	  if ( mmio->list->state == OCSE_DONE ) {
	    // we have read the response and the data (if any) already so just return
	    mmio->list = mmio->list->_next;
	    // debug_msg( "handle_ap_resp_data: removed cfg from list" );
	  }
	  // debug_msg( "handle_ap_resp_data: have a cfg, but state is not done: rc = %d", rc );
	  return;
	} else {
	  if (mmio->list->rnw) {
	    rc = afu_tlx_read_resp_dcp0_data( mmio->afu_event,
					 &resp_data_is_valid, rdata_bus, &rdata_bad);
	    // debug_msg( "handle_ap_resp_data: not cfg, but a mmio read: attempted a read of resp data: rc = %d", rc );
	  } else {
	    // no resp data to read
	    if ( mmio->list->state == OCSE_DONE ) {
	      mmio->list = mmio->list->_next;
	      // debug_msg( "handle_ap_resp_data: removed mmio/lpc write from list" );
	    }
	    return;
	  }
	
	  if (rc == TLX_SUCCESS) {
	      // we have some data for a read command
	      // check to make sure there is a BUFFERing mmio - if not, it is an error...
      	      // if there is a BUFFERing mmio and we didn't get data, it is an error...

	      // this section needs to handle lpc memory data
	      // send_mmio set mmio.ack field with the type of ack we need to send back to libocxl (mmio or lpc)
	      // we can leverage that to decide how do interpret the data and respective size information
	      // the data will always come in the 64 byte buffer.
	      // we only want to send the exact size of the data back to libocxl
	      // we get the data from the offset implied by the PA.

	      // should we scan the mmio list looking for a matching CAPPtag here? Not yet, assume in order responses
              // but we can check it...

	      debug_mmio_ack(mmio->dbg_fp, mmio->dbg_id);

	      // is mmio->list there or is it in the expected state
	      if (!mmio->list || (mmio->list->state != OCSE_BUFFER)) {
	      		warn_msg("handle_ap_resp_data: Unexpected resp data from AFU");
			return;
	      }

	      if (resp_data_is_valid) {
#ifdef DEBUG
    	            printf( "rdata_bus = 0x" );
		    for (i = 0; i < 64; i++) {
		      printf( "%02x", rdata_bus[i] );
		    }
		    printf( "\n" );
#endif	  

		    // calculate length.  
		    //    for lpc, we can just use mmio->list->size
		    //    for mmio, we use pL - maybe we could set up mmio->list->size even for the old mmio path - then this is always use the size...
		    if ( mmio->list->size == 0 ) {
		          // we have a mmio of either 32 or 64 bits
		          if (mmio->list->cmd_pL == 0x02) {
			        length = 4;
			  } else {
  			        length = 8;
			  }
		          // for a partial read, the data comes back at an offset in rdata_bus
		          offset = mmio->list->cmd_PA & 0x000000000000003F ;
			  memcpy( &mmio->list->cmd_data, &rdata_bus[offset], length );
			  mmio->list->state = OCSE_DONE;
			  debug_msg("%s: CMD RESP offset=%d length=%d data=0x%016lx", mmio->afu_name, offset, length, mmio->list->cmd_data );
			  mmio->list = mmio->list->_next;  // the mmio we just processed is pointed to by ...
			  // debug_msg( "handle_ap_resp_data: removed mmio read from list" );
		    } else {
		          if ( mmio->list->size < 64 ) {
			        // for a partial read, the data comes back at an offset in rdata_bus
			        offset = mmio->list->cmd_PA & 0x000000000000003F ;
			        memcpy( mmio->list->data, &rdata_bus[offset], mmio->list->size );
				mmio->list->state = OCSE_DONE;
			  } else {
			        // size will be 64, 128 or 256
			        length = 64;
				offset = 0;
				switch (mmio->list->resp_dL) {
				case 1:
				  // the size of the response is 64 bytes in 1 beat
				  // offset is a simple function of dP * length
				  // only one beat of data comes in, so we can forget partial_index
				  offset = mmio->list->resp_dP * length;
				  break;
				case 2:
				  // the size of the response is 128 bytes in 2 beats
				  // offset is a simple function of dP * 2 * length  plus the partial index
				  offset = ( mmio->list->resp_dP * ( 2 * length ) ) + mmio->list->partial_index;
				  break;
				case 3:
				  // the size of the response is 256 bytes in 4 beats
				  // offset is a simple function of partial_index
				  offset = mmio->list->partial_index;
				  break;
				default:
				  error_msg("UNEXPECTED resp_dL: %d received", mmio->list->resp_dL);
				}
				memcpy( &mmio->list->data[offset], rdata_bus, length );
				mmio->list->partial_index = mmio->list->partial_index + length;
				mmio->list->size_received = mmio->list->size_received + length;
				if ( mmio->list->size_received == mmio->list->size ) {
				      // we have all the data we expect
				      mmio->list->state = OCSE_DONE;
				}
			  }

			  if ( mmio->list->state == OCSE_DONE ) {
#ifdef DEBUG
			    debug_msg("%s: CMD RESP length=%d", mmio->afu_name, length );
			    printf( "mmio->list->data = 0x" );
			    for (i = 0; i < mmio->list->size; i++) {
			      printf( "%02x", mmio->list->data[i] );
			    }
			    printf( "\n" );
#endif	  
			    mmio->list = mmio->list->_next;
			    // debug_msg( "handle_ap_resp_data: removed lpc read from list" );
			  }
		    }

	      } // resp_data_is_valid

	  } // TLX_SUCCESS

	}
}

// check resp_dl and resp_dp versus the expected cmd_dl
// this will include responses to config commmands, mmio requests, and lpc memory requests
int _resp_dldp_is_legal(uint8_t cmd_dl, uint8_t resp_dl, uint8_t resp_dp)
{
  if ( cmd_dl == 0 ) { // partial read
    if (resp_dl == 1) {
      return 0;
    }
  } 
  
  if ( cmd_dl == 1 ) { // 64 byte read
    if (resp_dl == 1) {
      return 0;
    }
  } 

  if ( cmd_dl == 2 ) { // 128 byte read
    if (resp_dl == 1) { // non-matching dl, split response
      if ( ( resp_dp == 0 ) | ( resp_dp == 1 ) ) {
	return 0;
      }
    } 
    if ( resp_dl == 2 ) { // matching dl, single response
      if ( resp_dp == 0 ) {
	return 0;
      }
    }
  } 

  if ( cmd_dl == 3 ) { // 256 byte read
    if (resp_dl == 1) { // non-matching dl, split response
      if ( ( resp_dp == 0 ) | ( resp_dp == 1 ) | ( resp_dp == 2 ) | ( resp_dp == 3 ) ) {
	return 0;
      }
    } 
    if ( resp_dl == 2 ) { // non-matching dl, split response
      if ( ( resp_dp == 0 ) | ( resp_dp == 1 ) ) {
	return 0;
      }
    }
    if ( resp_dl == 3 ) { // matching dl, single response
      if ( resp_dp == 0 ) {
	return 0;
      }
    }
  }
  
  return 1;
}

// Handle kill_xlate_done responses coming from the afu
// this only covers responses to client/host initiated kill_xlate commmands (comes back over afu_tlx_vc3 interface, not vc0)
/*void handle_ap_killdone(struct mmio *mmio,struct client *client)
{
	int rc;
	uint8_t resp_opcode, resp_code;
	uint16_t resp_capptag;
	uint16_t cmd_actag, cmd_afutag;
	uint8_t  cmd_stream_id;
	uint16_t *respcapptag;
	uint8_t *buffer;
	uint8_t ack = OCSE_MMIO_ACK;
	int fd = client->fd;


	if (mmio->list->cmd_opcode !=  TLX_CMD_KILL_XLATE)
       		return;
	else {
	rc =  afu_tlx_read_resp_vc3(mmio->afu_event, 
  		    &resp_opcode,&cmd_stream_id, 
		    &cmd_afutag,&cmd_actag,
		     &resp_capptag, &resp_code);

	// No command ready 
	if (rc != TLX_SUCCESS)
		return;

//	debug_msg( "%s:COMMAND actag=0x%02x afutag=0x%04x cmd=0x%x  ",
//		   cmd->afu_name,
//		   cmd_actag,
//		   cmd_afutag,
//		   cmd_opcode);


			// Send kill xlate done response to client
		buffer = (uint8_t *) malloc(4);
		buffer[0] = (uint8_t) OCSE_XLATE_KILL_DONE;
		buffer[1] = (uint8_t) resp_code;
		respcapptag = (uint16_t *) & (buffer[2]);
		*respcapptag = htonll(resp_capptag);
		debug_msg("%s:KILL_XLATE_DONE   capptag=0x%02x resp_code=0x%2x", resp_capptag,
			   resp_code);
		if (put_bytes(client->fd, 4, buffer, cmd->dbg_fp, cmd->dbg_id,
			      event->context) < 0) {
			client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
		}
		mmio->list->state = OCSE_DONE;
		return;
	}
} */

// Handle ap responses coming from the afu
// this will include responses to config commmands, mmio requests, amo cmds cache enable/disable cmds and lpc memory requests
void handle_ap_resp(struct mmio *mmio)
{
	int rc;
	char type[7];
	uint8_t afu_resp_opcode, resp_dl, resp_dp, resp_data_is_valid, resp_code, rdata_bad;
	uint16_t resp_capptag;
	uint32_t cfg_read_data = 0;
	unsigned char   rdata_bus[64];
	int length;

#ifdef DEBUG
	int i;
#endif	  

	// debug_msg( "handle_ap_rep:" );
	// handle config, mmio, and lpc responses

	// a response can have multiple beats of data (depending on dl/dp/pl values) - similar to an ap command flow
	// The first beat of data is at the same time as the response.
	// the remaining beats of data will immediately follow the response
	// another response may overlap the remaining beats of data if this new response contains no data
	// TODO - response may be split, but lets not worry about that now.

	// if we are expecting a config response, they come as response and data together
	// capture the response and data and we're done
	// otherwise, just capture the repsone and prepare to recieve the data
	if (mmio->list->cfg) {
		if (mmio->list->rnw) {
			rc = afu_tlx_read_cfg_resp_and_data (mmio->afu_event,
							     &afu_resp_opcode, &resp_capptag, 0xdead,
							     &resp_data_is_valid, &resp_code, rdata_bus, &rdata_bad);
			// debug_msg( "handle_ap_resp: rc from afu_tlx_read_cfg_resp_and_data = %d", rc );
		} else {
			rc = afu_tlx_read_cfg_resp_and_data (mmio->afu_event,
							     &afu_resp_opcode, &resp_capptag, 0xbeef,
							     &resp_data_is_valid, &resp_code, 0, 0);
			// debug_msg( "handle_ap_resp: rc from afu_tlx_read_cfg_resp_and_data (no data expected) = %d", rc );
		}
	} else {
	        // we read the response, and prepare to read the data in a subsequent routine.
	        rc = afu_tlx_read_resp_vc0(mmio->afu_event,
				       &afu_resp_opcode, &resp_dl, &resp_capptag, &resp_dp, &resp_code);
		// debug_msg( "handle_ap_resp: rc from afu_tlx_read_resp = %d", rc );
	}

	if (rc == TLX_SUCCESS) {
	      //
              // at this point, either have 64 bytes of data in rdata_bus (for config), or we have set the mmio resp state to OCSE_BUFFER (a new state)
	      //
	      // should we scan the mmio list looking for a matching CAPPtag here? Not yet, assume in order responses
              // but we can check it...
	      debug_mmio_ack(mmio->dbg_fp, mmio->dbg_id);

	      // debug_msg( "handle_ap_resp: current event state = %d", mmio->list->state );

	      // make sure we have an mmio expecting a response
	      if (!mmio->list || (mmio->list->state != OCSE_PENDING)) {
	      		warn_msg("handle_ap_resp: Unexpected resp from AFU");
			return;
	      }

	      // check the CAPPtag - later
	      // TODO put this someplace better? for now, check first to see if this is a response to cache enable/disable
	      // and handle first!
	      switch(afu_resp_opcode) {
		      case AFU_RSP_CACHE_DISABLED:
		      case AFU_RSP_CACHE_ENABLED:
		      case AFU_RSP_ATC_DISABLED:
		      case AFU_RSP_ATC_ENABLED:
				mmio->list->resp_code = resp_code;
				mmio->list->cmd_CAPPtag = resp_capptag;
				mmio->list->resp_opcode = afu_resp_opcode;
		     		mmio->list->state = OCSE_DONE;
	      			debug_msg("handle_ap_resp: enable/disable cache/atc resp_capptag = %x and resp_code = %x! ", resp_capptag, resp_code);
				return;
		      default:
				break;
		}

	      if (mmio->list->cfg) {
		    sprintf(type, "CONFIG");
	      } else if ( mmio->list->size == 0 ) {
		    sprintf(type, "MMIO");
	      } else {
	            sprintf(type, "MEM");
	      }
	      debug_msg("handle_ap_resp: resp_capptag = %x and resp_code = %x! ", resp_capptag, resp_code);

	      mmio->list->resp_code = resp_code;  //save this to send back to libocxl/client
	      mmio->list->resp_opcode = afu_resp_opcode;  //save this to send back to libocxl/client

	      if (mmio->list->cfg) {
		if (resp_data_is_valid) {
		  // that is, we are processing a config...
#ifdef DEBUG
		  printf( "rdata_bus = 0x" );
		  for (i = 0; i < 64; i++) {
		    printf( "%02x", rdata_bus[i] );
		  }
		  printf( "\n" );
#endif	  
		  //cfg responses with data are only 4B, for now don't put into uint64_t, put in uint32_t 
		  //we will fix this later and use cfg_resp_data_byte cnt!
		  length = 4;
		  memcpy( &cfg_read_data, &rdata_bus[0], length );
		  debug_msg("%s:%s CFG CMD RESP  length=%d data=0x%08x code=0x%02x", mmio->afu_name, type, length, cfg_read_data, resp_code ); // ???
		} 
	      }

	      // Keep data for MMIO reads
	      if (mmio->list->rnw) {
		// debug_msg( "READ - stashing data" );
		if (mmio->list->cfg) {
		      // debug_msg( "CONFIG" );
		      mmio->list->cmd_data = (uint64_t) (cfg_read_data);
		      mmio->list->state = OCSE_DONE;
		      // mmio->list = mmio->list->_next;
		} else {
		  // debug_msg( "MMIO size > 0" );
		  if ( _resp_dldp_is_legal( mmio->list->cmd_dL, resp_dl, resp_dp ) == 1 ) {
		    error_msg("%s:%s PARTIAL MEMORY READ RESP: cmd dL %d received illegal resp dL/dP received %d/%d", 
			      mmio->afu_name, 
			      type, 
			      mmio->list->cmd_dL, 
			      resp_dl, 
			      resp_dp );
		  }
		  // save resp_dl and resp_dp to handle the split response insertion into the data buffer
		  mmio->list->resp_dL = resp_dl;
		  mmio->list->resp_dP = resp_dp;
		  mmio->list->partial_index = 0;
		  mmio->list->state = OCSE_BUFFER;
		}
	      } else {
		mmio->list->state = OCSE_DONE;
	      }
	}

}

// Handle MMIO ack if returned by AFU
/*void handle_mmio_ack(struct mmio *mmio)
{
	int rc;
	char type[7];
	uint8_t afu_resp_opcode, resp_dl,resp_dp, resp_data_is_valid, resp_code, rdata_bad;
	uint16_t resp_capptag;
	uint32_t cfg_read_data = 0;
        uint64_t read_data; // data can now be up to 64 bytes, not just upto 8
	uint8_t *  rdata;
	unsigned char   rdata_bus[64];
	unsigned char   cfg_rdata_bus[4];
	unsigned char   mem_data[64];
	int offset, length;

#ifdef DEBUG
	int i;
#endif	  

	// handle config and mmio responses
	// length can be calculated from the mmio->list->dw or cmd_pL
	// location of data in rdata_bus is address aligned based on mmio->list->cmd_PA
	// that is, mask off the high order address bits to form the offset - keep the low order 6 bits.

	length = 0;
	rdata = rdata_bus;

	// NEEDS MODIFICATION for > 64 Byte responses
	// a response can have multiple beats of data (depending on dl/dp/pl values) - similar to an ap command flow
	// The first beat of data is at the same time as the response.
	// the remaining beats of data will immediately follow the response
	// another response may overlap the remaining beats of data if this new response contains no data
	// for now, let's assume we get 1 response for a given command, so all the data for that request will arrive in
	// one response.  response may be split, but lets not worry about that now.
	
	if (mmio->list->cfg) {
		if (mmio->list->rnw) {
			rdata = cfg_rdata_bus;
			rc = afu_tlx_read_cfg_resp_and_data (mmio->afu_event,
							     &afu_resp_opcode, &resp_capptag, 0xdead, 
							     &resp_data_is_valid, &resp_code, rdata_bus, &rdata_bad);
		} else {
			rc = afu_tlx_read_cfg_resp_and_data (mmio->afu_event,
							     &afu_resp_opcode, &resp_capptag, 0xbeef,
							     &resp_data_is_valid, &resp_code, 0, 0);
		}

	} else {
	        // we probably need to split this into afu_tlx_read_resp and afu_tlx_read_resp_data so we can collect data beats.
	        rc = afu_tlx_read_resp_vc0_and_dcp0(mmio->afu_event,
						&afu_resp_opcode, &resp_dl,
						&resp_capptag, &resp_dp,
						&resp_data_is_valid, &resp_code, rdata_bus, &rdata_bad);
	}

	// this section needs to handle lpc memory data
	// send_mmio set mmio.ack field with the type of ack we need to send back to libocxl (mmio or lpc)
	// we can leverage that to decide how do interpret the data and respective size information
	// the data will always come in the 64 byte buffer.
	// we only want to send the exact size of the data back to libocxl
	// we get the data from the offset implied by the PA.
	if (rc == TLX_SUCCESS) {
	      //
              // at this point, we have 64 bytes of data in rdata_bus
	      //
	      // should we scan the mmio list looking for a matching CAPPtag here? Not yet, assume in order responses
              // but we can check it...
	      debug_mmio_ack(mmio->dbg_fp, mmio->dbg_id);
	      if (!mmio->list || (mmio->list->state != OCSE_PENDING)) {
	      		warn_msg("Unexpected MMIO ack from AFU");
			return;
	      }

	      // check the CAPPtag - later

	      if (mmio->list->cfg) {
		    sprintf(type, "CONFIG");
	      } else if ( mmio->list->size == 0 ) {
		    sprintf(type, "MMIO");
	      } else {
	            sprintf(type, "MEM");
	      }

	      debug_msg("IN handle_mmio_ack and resp_capptag = %x and resp_code = %x! ", resp_capptag, resp_code);
	      mmio->list->resp_code = resp_code;  //save this to send back to libocxl/client
	      mmio->list->resp_opcode = afu_resp_opcode;  //save this to send back to libocxl/client
	      if (resp_data_is_valid) {
#ifdef DEBUG
    	            printf( "rdata_bus = 0x" );
		    for (i = 0; i < 64; i++) {
		      printf( "%02x", rdata_bus[i] );
		    }
		    printf( "\n" );
#endif	  
		    if (mmio->list->cfg) {
		          //TODO data is only 4B, for now don't put into uint64_t, put in uint32_t 
		          //we will fix this lateri and use cfg_resp_data_byte cnt!
		          length = 4;
			  memcpy( &cfg_read_data, &rdata_bus[0], length );
			  debug_msg("%s:%s CFG CMD RESP  length=%d data=0x%08x code=0x%02x", mmio->afu_name, type, length, cfg_read_data, resp_code ); // ???
		    } else {
		          // if this is an lpc response, the data could be a number of sizes at varying offsets in rdata
		          // extract data from address aligned offset in vector - this might not work if size > 64...
		          offset = mmio->list->cmd_PA & 0x000000000000003F ;

			  // calculate length.  
			  //    for lpc, we can just use mmio->list->size if we want.  Or we can decode dl/dp
			  //    for mmio, we use pL - maybe we could set up mmio->list->size even for the old mmio path - then this is always use the size...
			  if ( mmio->list->size == 0 ) {
			    if (mmio->list->cmd_pL == 0x02) {
			      length = 4;
			    } else {
			      length = 8;
			    }
			    memcpy( &read_data, &rdata_bus[offset], length );
			    debug_msg("%s:%s CMD RESP offset=%d length=%d data=0x%016lx code=0x%x", mmio->afu_name, type, offset, length, read_data, resp_code );
			  } else {
			    length = mmio->list->size;
			    memcpy( mem_data, &rdata_bus[offset], length );
			    debug_msg("%s:%s CMD RESP offset=%d length=%d code=0x%x", mmio->afu_name, type, offset, length, resp_code );
#ifdef DEBUG
    	                    printf( "mem_data = 0x" );
			    for (i = 0; i < 64; i++) {
			      printf( "%02x", mem_data[i] );
			    }
			    printf( "\n" );
#endif	  
			  }
		    }
	      } else {
		    if ((afu_resp_opcode == 2) && ((resp_capptag == 0xdead) || 
			(resp_capptag == 0xcafe) || (resp_capptag == 0xefac))) {
		          printf("CFG/MMIO/MEM RD FAILED! afu_resp_opcode = 0x%x and resp_code = 0x%x \n",
				 afu_resp_opcode, resp_code);
		    	debug_msg("%s:%s CMD RESP code=0x%x", mmio->afu_name, type, resp_code);
		    }
		// do we get an ack back for write?
		    if ((afu_resp_opcode == 2) && ((resp_capptag == 0xbeef) || 
			(resp_capptag == 0xbead) || (resp_capptag == 0xdaeb))) {
		          printf("CFG/MMIO/MEM WR FAILED! afu_resp_opcode = 0x%x and resp_code = 0x%x \n",
				 afu_resp_opcode, resp_code);
		    	debug_msg("%s:%s CMD RESP code=0x%x", mmio->afu_name, type, resp_code);
		    }
	      }

	      // Keep data for MMIO reads
	      if (mmio->list->rnw) {
		// debug_msg( "READ - stashing data" );
		if (mmio->list->cfg) {
		      // debug_msg( "CONFIG" );
		      mmio->list->cmd_data = (uint64_t) (cfg_read_data);
		} else if ( mmio->list->size == 0 ) {
		      // debug_msg( "MMIO size 0?" );
                      mmio->list->cmd_data = read_data;
		} else {
		      // debug_msg( "MMIO size > 0" );
		      memcpy( mmio->list->data, mem_data, length );
#ifdef DEBUG
		      printf( "mmio->list->data = 0x" );
		      for (i = 0; i < 64; i++) {
			printf( "%02x", mmio->list->data[i] );
		      }
		      printf( "\n" );
#endif	  
		}
	      }
	      mmio->list->state = OCSE_DONE;
	      mmio->list = mmio->list->_next;
	}

} */

// Handle MMIO map request from client
void handle_mmio_map(struct mmio *mmio, struct client *client)
{
  // uint32_t flags;
	uint8_t *buffer;
	uint8_t ack = OCSE_MMIO_ACK;
	int fd = client->fd;

	if (ack == OCSE_MMIO_ACK) {
		debug_mmio_map(mmio->dbg_fp, mmio->dbg_id, client->context);
	}

	// map_done:
	// Send acknowledge to client	   
	buffer = (uint8_t *) malloc(2);
	buffer[0] = ack;
	buffer[1] = 0;
	if (put_bytes(fd, 2, buffer, mmio->dbg_fp, mmio->dbg_id, client->context) < 0) {
	  client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
	}

}

// Add mmio write event of register at offset to list
static struct mmio_event *_handle_mmio_write(struct mmio *mmio,
					     struct client *client, int dw, int global)
{
	struct mmio_event *event;
	uint32_t offset;
	uint64_t data64;
	uint32_t data32;
	uint64_t data;
	int fd = client->fd;

	if (get_bytes_silent(fd, 4, (uint8_t *) & offset, mmio->timeout,
			     &(client->abort)) < 0) {
		goto write_fail;
	}
	offset = ntohl(offset);
	if (dw) {
		if (get_bytes_silent(fd, 8, (uint8_t *) & data64, mmio->timeout,
				     &(client->abort)) < 0) {
			goto write_fail;
		}
		// Convert data from client from little endian to host
		data = ntohll(data64);
	} else {
		if (get_bytes_silent(fd, 4, (uint8_t *) & data32, mmio->timeout,
				     &(client->abort)) < 0) {
			goto write_fail;
		}
		// Convert data from client from little endian to host
		data32 = ntohl(data32);
		data = (uint64_t) data32;
		data <<= 32;
		data |= (uint64_t) data32;
	}

	event = _add_mmio(mmio, client, 0, dw, global, offset, data);
	return event;

 write_fail:
	// Socket connection is dead
	debug_msg("%s:_handle_mmio_write failed context=%d",
		  mmio->afu_name, client->context);
	client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
	return NULL;
}

// Add mmio read event of register at offset to list
static struct mmio_event *_handle_mmio_read(struct mmio *mmio,
					    struct client *client, int dw, int global)
{
	struct mmio_event *event;
	uint32_t offset;
	int fd = client->fd;

	if (get_bytes_silent(fd, 4, (uint8_t *) & offset, mmio->timeout,
			     &(client->abort)) < 0) {
		goto read_fail;
	}
	offset = ntohl(offset);

	event = _add_mmio(mmio, client, 1, dw, global, offset, 0);
	return event;

 read_fail:
	// Socket connection is dead
	debug_msg("%s:_handle_mmio_read failed context=%d",
		  mmio->afu_name, client->context);
	client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
	return NULL;
}



// Handle MMIO request from client
struct mmio_event *handle_mmio(struct mmio *mmio, struct client *client,
			       int rnw, int dw, int global)
{
	uint8_t ack;

	// Only allow MMIO access when client is valid
	if (client->state != CLIENT_VALID) {
		ack = OCSE_MMIO_FAIL;
		if (put_bytes(client->fd, 1, &ack, mmio->dbg_fp, mmio->dbg_id,
			      client->context) < 0) {
			client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
		}
		return NULL;
	}

	if (rnw)
		return _handle_mmio_read(mmio, client, dw, global);
	else
		return _handle_mmio_write(mmio, client, dw, global);
}

// Handle MMIO done
struct mmio_event *handle_mmio_done(struct mmio *mmio, struct client *client)
{
	struct mmio_event *this_event;
	struct mmio_event *prev_event = NULL;
	struct mmio_event *next_event = NULL;
	uint64_t data64;
	uint32_t data32;
	uint8_t *buffer;
	int fd = client->fd;

	// Is there an MMIO event pending for this client?
	this_event = (struct mmio_event *)client->mmio_access;
	if (this_event == NULL)
		return NULL;

	// Is the MMIO event this client is waiting for done yet?
	if (this_event->state != OCSE_DONE)
		return this_event;

	// we are assuming that host commands complete (become DONE) in order...
	struct mmio_event *list_event;
	debug_msg( "handle_mmio_done: client @ 0x%016llx mmio done @ 0x%016llx for client @ 0x%016llx, remove from:", client, this_event, this_event->client );
	list_event = mmio->list;
	while (list_event != NULL) {
	  debug_msg( "handle_mmio_done:      mmio @ 0x%016llx, _next @ 0x%016llx", list_event, list_event->_next );
	  list_event = list_event->_next;
	}

	// if AFU sent a mem_rd_fail or mem_wr_fail response, send them on to libocxl so it can interpret the resp_code
	// and retry if needed, or fail simulation 
	if (((this_event->resp_opcode == 0x02) || (this_event->resp_opcode == 0x04)) && (this_event->resp_code != 0))  {
	      debug_msg("handle mmio_done: sending OCSE_ACK for failed READ or WRITE to client");
	      buffer = (uint8_t *) malloc(2);
	      buffer[0] = this_event->ack;
	      buffer[1] = this_event->resp_code;
	      if (put_bytes(fd, 2, buffer, mmio->dbg_fp, mmio->dbg_id,
			      client->context) < 0) {
			client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
	      }
	      debug_mmio_return(mmio->dbg_fp, mmio->dbg_id, client->context);
	} else if ((this_event->rnw) || (this_event->cmd_opcode == OCSE_AFU_AMO_RW)) {
	        // Return acknowledge with read data
	        if ( this_event->size !=0 ) {
		        // this is an lpc mem request coming back
		        debug_msg("handle_mmio_done: sending OCSE_LPC_ACK for a AMO_R, AMO_RW, or READ to client!!!!");
			buffer = (uint8_t *) malloc(this_event->size + 2);
			buffer[0] = this_event->ack;
			buffer[1] = this_event->resp_code;
			memcpy( &(buffer[2]), this_event->data, this_event->size );
			if (put_bytes(fd, this_event->size + 2, buffer, mmio->dbg_fp, mmio->dbg_id, client->context) < 0) {
		          client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
			}
			free( this_event->data );
		} else if ( this_event->dw ) {
		        debug_msg("handle_mmio_done: sending OCSE_MMIO_ACK for a dw READ to client!!!!");
			buffer = (uint8_t *) malloc(10);
			buffer[0] = this_event->ack;
			buffer[1] = this_event->resp_code;
			data64 = htonll(this_event->cmd_data);
			memcpy(&(buffer[2]), &data64, 8);
			if (put_bytes(fd, 10, buffer, mmio->dbg_fp, mmio->dbg_id, client->context) < 0) {
		          client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
			}
		} else {
		        debug_msg("handle_mmio_done:sending OCSE_MMIO_ACK for a READ to client!!!!");
			buffer = (uint8_t *) malloc(6);
			buffer[0] = this_event->ack;
			buffer[1] = this_event->resp_code;
			data32 = htonl(this_event->cmd_data);
			memcpy(&(buffer[2]), &data32, 4);
			if (put_bytes(fd, 6, buffer, mmio->dbg_fp, mmio->dbg_id, client->context) < 0) {
		          client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
			}
		}
	} else if (this_event->cmd_opcode == OCSE_KILL_XLATE) {
	        // use the ea from the command to send back a resp based on receiving a kill_xlate_done
	        // ack(1), ea(8), resp_code(1)
		buffer = (uint8_t *) malloc(10);
		buffer[0] = this_event->ack;
		data64 = htonll( this_event->cmd_PA ); // ea
		memcpy( &(buffer[1]), &data64, 8 );		
		buffer[9] = this_event->resp_code;
		if (put_bytes(fd, 10, buffer, mmio->dbg_fp, mmio->dbg_id, client->context) < 0) {
		        client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
		}
		debug_msg("handle_mmio_done: KILL XLATE DONE SENT to HOST!!!!");
	} else if (this_event->cmd_opcode == OCSE_FORCE_EVICT) {
	        // no response necessary, a castout is now in flight to finish the evict
		debug_msg("handle_mmio_done: FORCE_EVICT being completed by CASTOUT!!!!");
	} else if ((this_event->cmd_opcode == OCSE_DISABLE_CACHE) || (this_event->cmd_opcode == OCSE_ENABLE_CACHE)
			|| (this_event->cmd_opcode == OCSE_DISABLE_ATC) || (this_event->cmd_opcode == OCSE_ENABLE_ATC)) {
		buffer = (uint8_t *) malloc(5);
		buffer[0] = this_event->ack;
		buffer[1] = this_event->resp_opcode;
		buffer[2] = this_event->resp_code;
		buffer[3] = this_event->cmd_CAPPtag; //TODO check this, may have to memcopy to get byte order correct?
		if (put_bytes(fd, 5, buffer, mmio->dbg_fp, mmio->dbg_id, client->context) < 0) {
		        client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
		}
		debug_msg("handle_mmio_done: ATC/CACHE ENABLED/DISABLED RESP  SENT to HOST!!!!");
	} else {
	        // Return acknowledge for write
		// debug_msg("READY TO SEND OCSE_*_ACK for a WRITE to client!!!!");
		buffer = (uint8_t *) malloc(2);
		buffer[0] = this_event->ack;
		buffer[1] = this_event->resp_code;
		if (put_bytes(fd, 2, buffer, mmio->dbg_fp, mmio->dbg_id, client->context) < 0) {
			client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
		}
		debug_msg("SENT OCSE_*_ACK for a WRITE to client!!!!");
	}

	// locate the pointer to this event in mmio->list and adjust the pointer to this events _next
	if (mmio->list != NULL) {
	  if (mmio->list == this_event) {
	    // event is the first in the list, update mmio->list to point to this_event->_next
	    mmio->list = this_event->_next;
	  } else {
	    //scan list for this event, and update the prev event _next pointer to skip this event
	    prev_event = mmio->list;
	    while (prev_event->_next != NULL) {
	      if (prev_event->_next == this_event) {
		prev_event->_next = this_event->_next;
	      }
	      prev_event = prev_event->_next;
	    }
	  }
	  // this_event is no longer in mmio->list
	}
	
	// free this_event and buffer
	debug_msg( "handle_mmio_done: completed mmio - freeing mmio @ 0x%016llx", this_event );
	free(this_event);
	free(buffer);

	// scan the list again, this time looking for another mmio that points back to this client
	debug_msg( "handle_mmio_done: find next mmio for client @ 0x%016llx in:", client );
	list_event = mmio->list;
	while (list_event != NULL) {
	  debug_msg( "handle_mmio_done:      mmio @ 0x%016llx for client @ 0x%016llx", list_event, list_event->client );
	  list_event = list_event->_next;
	}

	next_event = NULL;
	if (mmio->list != NULL) {
	  if (mmio->list->client == client) {
	    // the first mmio event belongs to this client
	    next_event = mmio->list;
	    debug_msg( "handle_mmio_done: found the next mmio for this client @ 0x%016llx @ 0x%016llx (it was the first one)", client, next_event );
	  } else {
	    // scan the list
	    this_event = mmio->list;
	    while (this_event->_next != NULL) {
	      if (this_event->_next->client == client) {
		next_event = this_event->_next;
		debug_msg( "handle_mmio_done: found the next mmio for this client @ 0x%016llx @ 0x%016llx (it was in the list)", client, next_event );
		break;
	      }
	      this_event = this_event->_next;
	    }
	  }
	}
	
	debug_mmio_return(mmio->dbg_fp, mmio->dbg_id, client->context);

	return next_event; // the mmio_event that we found in the list that points back to this client
	// return NULL;
}

// Add mem write event to offset in memory space
static struct mmio_event *_handle_mem_write(struct mmio *mmio, struct client *client, int region, int be_valid)
{
	struct mmio_event *event;
	uint32_t offset;
	uint32_t size;
	uint64_t be;
	uint8_t *data;
	int fd = client->fd;

	// get offset from socket
	if (get_bytes_silent(fd, 4, (uint8_t *)&offset, mmio->timeout,
			     &(client->abort)) < 0) {
		goto write_fail;
	}
	offset = ntohl(offset);

	if (be_valid == 0) {
	  // get size from socket
	  if (get_bytes_silent(fd, 4, (uint8_t *)&size, mmio->timeout,
			       &(client->abort)) < 0) {
	    goto write_fail;
	  }
	  size = ntohl(size);
	  be = 0;
	} else {
	  // get byte_enable from socket (size is always 64)
	  if (get_bytes_silent(fd, 8, (uint8_t *)&be, mmio->timeout,
			       &(client->abort)) < 0) {
	    goto write_fail;
	  }
	  be = ntohl(be);
	  size = 64;
	}	  

	// allocate a buffer for the data
	data = (uint8_t *)malloc( size );

	// get size bytes of data from socket
	if ( get_bytes_silent( fd, size, data, mmio->timeout, &(client->abort) ) < 0 ) {
	  goto write_fail;
	}

	event = _add_mem( mmio, client, 0, size, region, offset, data, be_valid, be );

	return event;

 write_fail:
	// Socket connection is dead
	debug_msg("%s:_handle_mmio_write failed context=%d",
		  mmio->afu_name, client->context);
	client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
	return NULL;
}

// Add mmio read event of register at offset to list
static struct mmio_event *_handle_mem_read(struct mmio *mmio, struct client *client, int region)
{
	struct mmio_event *event;
	uint32_t offset;
	uint32_t size;
	uint8_t *data;
	int fd = client->fd;

	if (get_bytes_silent(fd, 4, (uint8_t *) &offset, mmio->timeout, &(client->abort)) < 0) {
		goto read_fail;
	}
	offset = ntohl(offset);

	if (get_bytes_silent(fd, 4, (uint8_t *) &size, mmio->timeout, &(client->abort)) < 0) {
		goto read_fail;
	}
	size = ntohl(size);

	// allocate a buffer for the data coming back
	data = (uint8_t *)malloc( size );

	event = _add_mem( mmio, client, 1, size, region, offset, data, 0, 0 );

	return event;

 read_fail:
	// Socket connection is dead
	debug_msg("%s:_handle_mmio_read failed context=%d",
		  mmio->afu_name, client->context);
	client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
	return NULL;
}

// Handle kill xlate request from client
struct mmio_event *handle_kill_xlate(struct mmio *mmio, struct client *client)
{

        //uint8_t ack;
	uint8_t cmd_flag;
	uint8_t pg_size;
	uint16_t bdf;
	uint32_t pasid;
	uint64_t ea;
	// client->fd is the socket that contains the rest of the kill xlate components
	// build the kill xlate command.

	// get cmd_flag
	if (get_bytes_silent(client->fd, 1, (uint8_t *) &cmd_flag, mmio->timeout, &(client->abort)) < 0) {
		goto kill_fail;
	}

	// get ea
	if (get_bytes_silent(client->fd, 8, (uint8_t *) &ea, mmio->timeout, &(client->abort)) < 0) {
		goto kill_fail;
	}
	ea = ntohll( ea );

	// get pg_size
	if (get_bytes_silent(client->fd, 1, (uint8_t *) &pg_size, mmio->timeout, &(client->abort)) < 0) {
		goto kill_fail;
	}

	// get bdf
	if (get_bytes_silent(client->fd, 2, (uint8_t *) &bdf, mmio->timeout, &(client->abort)) < 0) {
		goto kill_fail;
	}
	bdf = ntohs( bdf );

	// get pasid
	if (get_bytes_silent(client->fd, 4, (uint8_t *) &pasid, mmio->timeout, &(client->abort)) < 0) {
		goto kill_fail;
	}
	pasid = ntohl( pasid );

	debug_msg( "handle_kill_xlate: ea=0x%016lx, pg_size=0x%02x, cmd_flag=0x%1x, bdf=0x%02x, pasid=0x%04x", 
		   ea, pg_size, cmd_flag, bdf, pasid );

	return add_kill_xlate_event( mmio, client, ea, pg_size, cmd_flag, bdf, pasid );

 kill_fail:
	// Socket connection is dead
	debug_msg("%s:handle_kill_xlate failed context=%d",
		  mmio->afu_name, client->context);
	client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
	return NULL;
}

// Handle mem request from client
struct mmio_event *handle_mem(struct mmio *mmio, struct client *client,
			      int rnw, int region, int be_valid)
{
	uint8_t ack;

	debug_msg( "handle_mem: rnw=%d", rnw );

	// Only allow mem access when client is valid
	if (client->state != CLIENT_VALID) {
	        debug_msg( "_handle_mem: invalid client" );
		ack = OCSE_LPC_FAIL;
		if (put_bytes(client->fd, 1, &ack, mmio->dbg_fp, mmio->dbg_id,
			      client->context) < 0) {
			client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
		}
		return NULL;
	}

	if (rnw)
		return _handle_mem_read(mmio, client, region);
	else
	        return _handle_mem_write(mmio, client, region, be_valid);
}

// create a new _add_afu_amo_event function that will use size instead of dw.
// Add new mmio event for sending amo cmds to afu
static struct mmio_event *_add_afu_amo_event(struct mmio *mmio, struct client *client,
				    uint32_t rnw, uint32_t size, int region, uint64_t addr,
				   uint8_t cmd_flg, uint8_t cmd, uint8_t *data, uint8_t *dataw, uint8_t endian)
{
	struct mmio_event *event;
	struct mmio_event **list;
	uint8_t fcn; 
	uint8_t afuid;
	uint16_t context;

	// Add new event in IDLE state
	event = (struct mmio_event *)calloc(1, sizeof(struct mmio_event));
	if (!event)
		return event;
	event->cfg = 0;
	event->rnw = rnw;
	event->dw = 0;
	event->size = size;  // part of the new fields
	event->data = data;
	event->dataw = dataw;
	event->cmd_flg = cmd_flg;
	event->cmd_endian = endian;
	event->cmd_opcode = cmd;
	if (client == NULL)  {
	  // TODO this should be an error exit or at least return an error
	  error_msg("WE SHOULD NOT BE HERE in add_afu_amo_cmd with client==NULL");
	  //event->cmd_PA = addr;
	} else {
	  // for OpenCAPI, the memory space is split into LPC, global and per pasid
	  // the region parm controls how we adjust the offset prior to adding the event
	  // technically, all of these should be adjusted by the BAR specified in the configuration... ocse assumes a BA of 0
	  //   region = 0 means we are LPC memory and offset is unadjusted
	  //   region = 1 means we adjust offset based on the global mmio offset from the configuration
	  //   region = 2 means we want to send the offset adjusted by the per pasid mmio offset, per pasid mmio stride, and client index
	  //   for now, we are assuming the client index (context) maps directly to a pasid.  
	  //        we could be more creative and relocate the pasid base and pasid length supported to 
	  //        provide more verification coverage
	  fcn = client->fcn;
	  afuid = client->afuid;

	  if (region == 0) {
	    // lpc area
	    event->cmd_PA = addr;
	  } else if (region == 1) {
	    // global mmio offset + offset
	    event->cmd_PA = mmio->fcn_cfg_array[fcn]->afu_cfg_array[afuid]->global_mmio_offset + addr;
	  } else {
	    // per pasid mmio offset + (client context * stride) + offset
	    event->cmd_PA = 
	      mmio->fcn_cfg_array[fcn]->afu_cfg_array[afuid]->pp_mmio_offset + 
	      ( mmio->fcn_cfg_array[fcn]->afu_cfg_array[afuid]->pp_mmio_stride * client->context) + 
	      addr;
	  }
	}
	event->state = OCSE_IDLE;
	event->_next = NULL;

	debug_msg("_add_afu_amo_event: rnw=%d, access word=0x%016lx (0x%016lx) cmd_flg= 0x%x", event->rnw, event->cmd_PA, addr, cmd_flg );
#ifdef DEBUG
	printf("_add_afu_amo_event: data = 0x" );
	int i;
	for ( i=0; i<size; i++ ) {
	  printf( "%02x", event->data[i] );
	}
	printf( "\n" );
#endif

	// Add to end of list
	list = &(mmio->list);
	while (*list != NULL)
		list = &((*list)->_next);
	*list = event;
	if (event->cfg)
		context = -1;
	else
		context = client->context;
	debug_mmio_add(mmio->dbg_fp, mmio->dbg_id, context, rnw, size, addr);

	return event;
}


// Handle amo_rw, amo_rw or amo_wr request from client
struct mmio_event *handle_afu_amo(struct mmio *mmio, struct client *client,
			      int rnw, int region, int cmd)
{
	struct mmio_event *event;
	uint64_t PA;
	uint32_t offset;
	uint32_t size;
	uint8_t cmd_flg;
	uint8_t *datav;
	uint8_t *dataw;
	uint8_t endian;
	int fd = client->fd;

	uint8_t ack;

	debug_msg( "handle_afu_amo: rnw=%d", rnw );

	// Only allow mem access when client is valid
	if (client->state != CLIENT_VALID) {
	        debug_msg( "_handle_afu_amo: invalid client" );
		ack = OCSE_LPC_FAIL;
		if (put_bytes(client->fd, 1, &ack, mmio->dbg_fp, mmio->dbg_id,
			      client->context) < 0) {
			client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
		}
		return NULL;
	}



	if (get_bytes_silent(fd, 4, (uint8_t *) &offset, mmio->timeout, &(client->abort)) < 0) {
		goto amo_fail;
	}
	PA = ntohl(offset);

	if (get_bytes_silent(fd, 4, (uint8_t *) &size, mmio->timeout, &(client->abort)) < 0) {
		goto amo_fail;
	}
	size = ntohl(size);

	if (get_bytes_silent(fd, 1, (uint8_t *) &cmd_flg, mmio->timeout, &(client->abort)) < 0) {
		goto amo_fail;
	}

	if (get_bytes_silent(fd, 1, (uint8_t *) &endian, mmio->timeout, &(client->abort)) < 0) {
		goto amo_fail;
	}

	// data
	// for an amo_r, just allocate a buffer (datav)
	// for an amo_w, allocate a buffer and fill it. (datav)
	// for an amo_rw, it gets tricky. allocate 2 buffers, datav and dataw. v will double as the return buffer

	if ( cmd != OCSE_AFU_AMO_RW ) {
	  // normal
	  if (rnw) {
	    // if a read, allocate the return buffer and build the event
	    datav = (uint8_t *)malloc( size );
	    event = _add_afu_amo_event( mmio, client, 1, size, region, PA, cmd_flg, cmd, datav, NULL, endian);
	  } else {
	    // if a write, allocate a data buffer, fill it from the socket and build the event
	    datav = (uint8_t *)malloc( size );
	    if ( get_bytes_silent( fd, size, datav, mmio->timeout, &(client->abort) ) < 0 ) {
	      goto amo_fail;
	    }
	    event = _add_afu_amo_event( mmio, client, 0, size, region, PA, cmd_flg, cmd, datav, NULL, endian);
	  }
	} else {
	  // special
	  // cmd_flag 0-10 have a datav
	  // always allocate datav, fill it in most cases.  always used for return data.
	  datav = (uint8_t *)malloc( size );
	  //the if statement below means that clients sending amo_rw cmds with cmd_flg = 0x8 must ALWAYS send NULL ptr for datav 
	  //otherwise, libocxl will send over datav data on socket, we'll not read it, and ocse will HANG/fail on next socket
	  //transaction after this one
	  if ( ( (cmd_flg >= 0) && (cmd_flg <= 7)  ) || 
	        (cmd_flg == 9) || (cmd_flg == 10)  ) {
	    if ( get_bytes_silent( fd, size, datav, mmio->timeout, &(client->abort) ) < 0 ) {
	      goto amo_fail;
	    }
	  }
	  // cmd_flag 8-10 have a dataw
	  dataw = NULL;
	  if ( (cmd_flg >= 8) && (cmd_flg <= 10)  ) {
	  debug_msg(" cmd has a dataw, getting it now size=%d cmd_flag=%d", size, cmd_flg);
	    dataw = (uint8_t *)malloc( size );
	    if ( get_bytes_silent( fd, size, dataw, mmio->timeout, &(client->abort) ) < 0 ) {
	      goto amo_fail;
	    }
	  }
	  event = _add_afu_amo_event( mmio, client, 0, size, region, PA, cmd_flg, cmd, datav, dataw, endian);
	}

	return event;

 amo_fail:
	// Socket connection is dead
	debug_msg("%s:_handle_afu_amo failed context=%d",
		  mmio->afu_name, client->context);
	client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE);
	return NULL;
}

// Handle request from client to send various cache management cmds to AFU 
 struct mmio_event *handle_capp_cache_cmd(struct mmio *mmio, struct client *client) 
 { 
 	struct mmio_event *event; 
 	struct mmio_event **list; 
 	uint32_t host_tag; 
 	uint16_t bdf; 
 	uint16_t pasid; 
 	uint16_t CAPP_tag; 
 	uint8_t cmd_flg; 
 	uint8_t cmd_dL; 
 	uint8_t cmd_opcode; 
 	uint16_t context; 
 	int fd = client->fd; 

 	uint8_t ack; 

 	debug_msg( "handle_capp_cache_cmd" ); 

 	// Only allow mem access when client is valid 
 	if (client->state != CLIENT_VALID) { 
 	        debug_msg( "handle_capp_cache_cmd: invalid client" ); 
 		ack = OCSE_KILL_XLATE_FAIL; 
 		if (put_bytes(client->fd, 1, &ack, mmio->dbg_fp, mmio->dbg_id,
 			      client->context) < 0) { 
 			client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE); 
 		} 
 		return NULL; 
 	} 


 	event = (struct mmio_event *)malloc(sizeof(struct mmio_event)); 
	if (!event)
		return event;
	// All 5 cmds expect to get the cmd_opcode sent over
 	if (get_bytes_silent(fd, 1, (uint8_t *) &cmd_opcode, mmio->timeout, &(client->abort)) < 0) { 
 		goto cache_cmd_fail; 
 	} 
 	event->cmd_opcode = cmd_opcode;
 	event->cmd_flg = 0; 
 	event->cmd_bdf = 0; 
 	event->cmd_pasid = 0; 
 	event->cmd_CAPPtag = 0; 
 	event->cmd_dL = 0; 
 	event->cmd_host_tag = 0; 
 	event->size = 0;  // part of the new fields 
 	event->size_received = 0;  // part of the new fields 

	if (cmd_flg != TLX_CMD_FORCE_EVICT) { // Only enable/disable atc/cache cmds need cmd_flag & bdf
 		if (get_bytes_silent(fd, 1, (uint8_t *) &cmd_flg, mmio->timeout, &(client->abort)) < 0) { 
 			goto cache_cmd_fail; 
 		} 
 		event->cmd_flg = cmd_flg; 

 		if (get_bytes_silent(fd, 2, (uint8_t *) &bdf, mmio->timeout, &(client->abort)) < 0) { 
 		 	goto cache_cmd_fail; 
 		} 
 		bdf = ntohl(bdf); 

 		if (get_bytes_silent(fd, 2, (uint8_t *) &pasid, mmio->timeout, &(client->abort)) < 0) { 
 			goto cache_cmd_fail; 
 		} 
 		pasid = ntohl(pasid); 
	}
	// All 5 cmds expect to get the CAPP_tag sent over
 	if (get_bytes_silent(fd, 2, (uint8_t *) &CAPP_tag, mmio->timeout, &(client->abort)) < 0) { 
 		goto cache_cmd_fail; 
 	} 
 	host_tag = ntohl(CAPP_tag); 

	if (cmd_flg == TLX_CMD_FORCE_EVICT) { // enable/disable atc/cache cmds don't need dL and host_tag
 		if (get_bytes_silent(fd, 1, (uint8_t *) &cmd_dL, mmio->timeout, &(client->abort)) < 0) { 
 			goto cache_cmd_fail; 
 		} 
		// This is ugly, but needed since the code parsing/sending mmio cmds expects to see non zero size
		// maybe have libocxl send over size instead of dL?
		if (cmd_dL == 1)
			event->size= 64;
		if (cmd_dL == 2)
			event->size= 128;

 		if (get_bytes_silent(fd, 4, (uint8_t *) &host_tag, mmio->timeout, &(client->abort)) < 0) { 
 			goto cache_cmd_fail; 
 		} 
 	host_tag = ntohl(host_tag); 
	}

 	event->cfg = 0; 
 	event->rnw = 0; 
 	event->be_valid = 0; 
 	event->dw = 0; 
 	event->data = 0; 
 	event->be = 0; 
 	event->cmd_ea = 0; 

 	event->state = OCSE_IDLE; 
 	event->_next = NULL;
 	debug_msg("_handle_capp_cache_cmd: cmd_opcode=0x%x  cmd_flg= 0x%x", event->cmd_opcode,  event->cmd_flg ); 
 		// Add to end of list 
 	list = &(mmio->list); 
 	while (*list != NULL) 
 		list = &((*list)->_next); 
 	*list = event; 
 	context = client->context; 
 	debug_mmio_add(mmio->dbg_fp, mmio->dbg_id, context, 0, cmd_flg,  event->cmd_ea); 

 	return event; 

  cache_cmd_fail: 
 	// Socket connection is dead 
 	debug_msg("%s:handle_capp_cache_cmd failed context=%d", 
 		  mmio->afu_name, client->context); 
 	client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE); 
	free(event);
 	return NULL; 

 } 

// Handle request from client to send various cache management cmds to AFU 
 struct mmio_event *handle_force_evict(struct mmio *mmio, struct client *client) 
 { 
 	struct mmio_event *event; 
 	struct mmio_event *last_mmio; 
	struct mmio_event *this_mmio;
 	// struct mmio_event **list; 
 	uint32_t host_tag; 
 	uint16_t size; 
 	int fd = client->fd; 

 	uint8_t ack; 

 	debug_msg( "handle_force_evict:" ); 

 	// Only allow mem access when client is valid 
 	if (client->state != CLIENT_VALID) { 
 	        debug_msg( "handle_capp_cache_cmd: invalid client" ); 
 		ack = OCSE_LPC_FAIL; 
 		if (put_bytes(client->fd, 1, &ack, mmio->dbg_fp, mmio->dbg_id,
 			      client->context) < 0) { 
 			client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE); 
 		} 
 		return NULL; 
 	} 

 	event = (struct mmio_event *)calloc(1, sizeof(struct mmio_event)); 
	if (!event) return event;

	event->client = client;
 	event->cmd_opcode = OCSE_FORCE_EVICT;
 	event->cmd_flg = 0; 
 	event->cmd_bdf = 0; 
 	event->cmd_pasid = 0; 
 	event->cmd_dL = 0; 
 	event->cmd_host_tag = 0; 
 	event->size = 0;  // part of the new fields 
 	event->size_received = 0;  // part of the new fields 

	// pull host_tag
 	if ( get_bytes_silent( fd, 4, (uint8_t *)&host_tag, mmio->timeout, &(client->abort) ) < 0 ) { 
	  goto handle_force_evict_fail; 
 	} 
 	event->cmd_host_tag = ntohl(host_tag); 

	// ocse adds the capp_tag
	event->cmd_CAPPtag = mmio->CAPPtag_next++;

	// pull size
 	if ( get_bytes_silent( fd, 2, (uint8_t *)&size, mmio->timeout, &(client->abort) ) < 0 ) { 
	  goto handle_force_evict_fail; 
 	} 
 	event->size = ntohs(size); 

 	event->cfg = 0; 
 	event->rnw = 0; 
 	event->be_valid = 0; 
 	event->dw = 0; 
 	event->data = 0; 
 	event->be = 0; 
 	event->cmd_ea = 0; 

 	event->state = OCSE_IDLE; 
 	event->_next = NULL;
 	debug_msg("handle_force_evict: client @ 0x%016llx, new mmio event @ 0x%016llx, cmd_opcode=0x%02x, host_tag=0x%06x, size=%d", event->client, event, event->cmd_opcode, event->cmd_host_tag, event->size ); 

	// Add to end of list
	last_mmio = NULL;
	this_mmio = mmio->list;
	
	while ( this_mmio != NULL ) {
	        debug_msg( "handle_force_evict: mmio event @ 0x%016llx, cmd_opcode=0x%02x, cmd_CAPPtag=0x%04x host_tag=0x%06x", 
			   this_mmio, this_mmio->cmd_opcode, this_mmio->cmd_CAPPtag, this_mmio->cmd_host_tag );
		last_mmio = this_mmio;
		this_mmio = this_mmio->_next; 
	}
	if (last_mmio == NULL) {
	  mmio->list = event;
	} else {
	  last_mmio->_next = event;
	}
	debug_msg( "handle_force_evict: mmio event @ 0x%016llx, cmd_opcode=0x%02x, cmd_CAPPtag=0x%04x host_tag=0x%06x", 
		   event, event->cmd_opcode, event->cmd_CAPPtag, event->cmd_host_tag );
 	debug_mmio_add(mmio->dbg_fp, mmio->dbg_id, client->context, 0, event->cmd_host_tag,  event->cmd_ea); 

 	return event; 

  handle_force_evict_fail: 
 	// Socket connection is dead 
 	debug_msg("%s:handle_force_evict: failed context=%d", mmio->afu_name, client->context); 
 	client_drop(client, TLX_IDLE_CYCLES, CLIENT_NONE); 
	free(event);
 	return NULL; 

 } 

