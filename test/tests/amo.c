/*
 * Copyright 2015,2017 International Business Machines
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

#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <inttypes.h>
#include "TestAFU_config.h"
#include "tlx_interface_t.h"
//#include "../../libocxl/libocxl.h"
#include <time.h>

#define ProcessControl_REGISTER 0x0018
#define PROCESS_CONTROL_RESTART 0x0000000000000001
#define ProcessInterruptControl_REGISTER 0x0020
#define ProcessInterruptObject_REGISTER 0x0028
#define ProcessInterruptData_REGISTER 0x0030

#define CACHELINE 128
#define MDEVICE "/dev/cxl/tlx0.0000:00:00.1.0"
#define SDEVICE "/dev/cxl/tlx0,000:00:00.2.0"
#define NAME    "IBM,MAFU"
#define PHYSICAL_FUNCTION   "1234:00:00.1"
#define WED_REGISTER 0x0000 
#define AFU_MMIO_REG_SIZE   0x4000000
#define AFU_CONFIGURATION_USE_PE_WED    0x8000000000000000

static int verbose;
static unsigned int buffer_cl = 64;
static unsigned int timeout   = 1;

struct work_element {
  uint8_t  command_byte; // left to right - 7:2 cmd, 1 wrap, 0 valid
  uint8_t  status;
  uint16_t length;
  uint8_t  command_extra;
  uint8_t  UNUSED_5;
  uint16_t UNUSED_6to7;
  uint64_t atomic_op1;
  uint64_t source_ea; // or atomic_op2
  uint64_t dest_ea;
};

static void print_help(char *name)
{
    printf("\nUsage:  %s [OPTIONS]\n", name);
    printf("\t--cachelines\tCachelines to copy.  Default=%d\n", buffer_cl);
    printf("\t--timeout   \tDefault=%d seconds\n", timeout);
    printf("\t--verbose   \tVerbose output\n");
    printf("\t--help      \tPrint Usage\n");
    printf("\n");
}

int main(int argc, char *argv[])
{
    struct timespec t;
    int opt, option_index, i;
    int rc;
    char adata[16];
    char *rcacheline, *wcacheline;
    char *status;
    ocxl_afu_h mafu_h, safu_h;
    ocxl_irq_h irq_h;
    //ocxl_irq_h err_irq_h;
    ocxl_event event;
    MachineConfig machine_config;
    MachineConfigParam config_param;
    uint64_t irq_id;
    uint64_t unused_flags;
    ocxl_mmio_h pp_mmio_h, pocxl_mmio_h;
    struct work_element *work_element_descriptor = 0;

    static struct option long_options[] = {
	{"cachelines", required_argument, 0	  , 'c'},
	{"timeout",    required_argument, 0	  , 't'},
	{"verbose",    no_argument      , &verbose,   1},
	{"help",       no_argument      , 0	  , 'h'},
	{NULL, 0, 0, 0}
    };

    while((opt = getopt_long(argc, argv, "vhc:t:", long_options, &option_index)) >= 0 )
    {
	switch(opt)
	{
	    case 'v':
	  	break;
	    case 'c':
		buffer_cl = strtoul(optarg, NULL, 0);
		break;
	    case 't':
		timeout = strtoul(optarg, NULL, 0);
		break;
	    case 'h':
		print_help(argv[0]);
		return 0;
	    default:
		print_help(argv[0]);
		return 0;
	}
    }

    t.tv_sec = 0;
    t.tv_nsec = 100000;
    // initialize machine
    init_machine(&machine_config);

    // align and randomize cacheline
    if (posix_memalign((void**)&rcacheline, CACHELINE, CACHELINE) != 0) {
	perror("FAILED: posix_memalign for rcacheline");
	goto done;
    }
    if (posix_memalign((void**)&wcacheline, CACHELINE, CACHELINE) != 0) {
	perror("FAILED: posix_memalign for wcacheline");
	goto done;
    }
    if(posix_memalign((void**)&status, 128, 128) != 0) {
	perror("FAILED: posix_memalign for status");
	goto done;
    }

    //printf("rcacheline = 0x");
    //for(i=0; i<CACHELINE; i++) {
	//rcacheline[i] = rand();
	//wcacheline[i] = 0x0;
	status[i] = 0x0;
	//printf("%02x", (uint8_t)rcacheline[i]);
    //}
    printf("\n");
    printf("Prep data for Fetch and incremented bounded test\n");
    memcpy(adata,"a0a0b1b1c2c2", 12);
    rcacheline = adata + 4;
    printf("%s\n", rcacheline);
    //status[0]=0xff;
    // open master device
    printf("Attempt open device for mafu\n");
    
    //rc = ocxl_afu_open_from_dev(MDEVICE, &mafu_h);
    rc = ocxl_afu_open_specific(NAME, PHYSICAL_FUNCTION, 0, &mafu_h);
    if(rc != 0) {
	perror("cxl_afu_open_dev: for mafu"MDEVICE);
	return -1;
    }
     
    // attach device
    printf("Attaching mafu device ...\n");
    rc = ocxl_afu_attach(mafu_h, 0);
    if(rc != 0) {
	perror("cxl_afu_attach:"MDEVICE);
	return rc;
    }

    printf("Attempt mmio mapping afu registers\n");
    if (ocxl_mmio_map(mafu_h, OCXL_PER_PASID_MMIO, &pp_mmio_h) != 0) {
	printf("FAILED: ocxl_mmio_map mafu\n");
	goto done;
    }
        printf("Attempt AMO Read command\n");
    config_param.context = 0;
    config_param.enable_always = 1;
    config_param.mem_size = CACHELINE;
    config_param.command = AFU_CMD_AMO_RD;
    config_param.mem_base_address = (uint64_t)rcacheline;
    config_param.machine_number = 0;
    config_param.status_address = (uint32_t)status;
    config_param.cmdflag = 0x0c;
    config_param.oplength = 0x02;
    printf("status address = 0x%p\n", status);
    printf("rcacheline = 0x%p\n", rcacheline);
    printf("command = 0x%x\n", config_param.command);
    printf("mem base address = 0x%"PRIx64"\n", config_param.mem_base_address);
    rc = config_enable_and_run_machine(mafu_h, pp_mmio_h, &machine_config, config_param, DIRECTED);
    printf("set status data = 0xff\n");
    status[0] = 0xff;
    if( rc != -1) {
	   printf("Response = 0x%x\n", rc);
	   printf("config_enable_and_run_machine PASS\n");
    }  
    else {
	   printf("FAILED: config_enable_and_run_machine\n");
	   goto done;
    }
    timeout = 0;
    printf("Waiting for read command completion status\n");
    while(status[0] != 0x0) {
	   nanosleep(&t, &t);
	   //printf("Polling read completion status = 0x%x\n", *status);
    }
    printf("Read command is completed\n");
    // clear machine config
    rc = clear_machine_config(pp_mmio_h, &machine_config, config_param, DIRECTED);
    if(rc != 0) {
	printf("Failed to clear machine config\n");
	goto done;
    }
    // Attemp write command
    printf("Attempt Write command\n");
    //status[0] = 0xff;
    config_param.context = 0;
    config_param.enable_always = 1;
    config_param.command = AFU_CMD_AMO_W;
    config_param.mem_size = 64;
    config_param.mem_base_address = (uint64_t)wcacheline;
    printf("wcacheline = 0x%p\n", wcacheline);
    printf("command = 0x%x\n",config_param.command);
    printf("wcache address = 0x%"PRIx64"\n", config_param.mem_base_address);
    rc = config_enable_and_run_machine(mafu_h, pp_mmio_h, &machine_config, config_param, DIRECTED);
    //status[0] = 0xff;
    if(rc != -1) {
	   printf("Response = 0x%x\n", rc);
 	  printf("config_enable_and_run_machine PASS\n");
    }
    else {
	   printf("FAILED: config_enable_and_run_machine\n");
	   goto done;
    }
    printf("set status data = 0xff\n");
    status[0] = 0xff;
    printf("Waiting for write command completion status\n");
    while(status[0] != 0x00) {
	   nanosleep(&t, &t);
	//printf("Polling write completion status = 0x%x\n", *status);
    }
    printf("Write command is completed\n");
    // clear machine config
    printf("Clear Machine Config\n");
    rc = clear_machine_config(pp_mmio_h, &machine_config, config_param, DIRECTED);
    if(rc != 0) {
	printf("Failed to clear machine config\n");
	goto done;
    }

    printf("wcacheline = 0x");
    for(i=0; i<CACHELINE; i++) {
	   printf("%02x", (uint8_t)wcacheline[i]);
    }
    printf("\n");

    printf("set status data = 0x55\n");
    status[0] = 0x55;	// test complete flag
    printf("Waiting for test complete status\n");
    while(status[0] != 0x0) {
	   nanosleep(&t, &t);
    }
    printf("Test is completed\n");
    printf("Attempt open device for safu\n");
    //rc = ocxl_afu_open_from_dev(MDEVICE, &safu_h);
    rc = ocxl_afu_open_specific(NAME, PHYSICAL_FUNCTION, 0, &safu_h);
    if(rc != 0) {
	   perror("cxl_afu_open_dev: for safu"MDEVICE);
	   return -1;
    }
    printf("Attaching safu device ....\n");
    rc = ocxl_afu_attach(safu_h, 0);
    if(rc != 0) {
	   perror("cxl_afu_attach: for safu"MDEVICE);
	   return -1;
    }
    printf("Attempt mmio map safu registers\n");
    if(ocxl_mmio_map(safu_h, OCXL_PER_PASID_MMIO, &pp_mmio_h) != 0) {
	   printf("AFILED: ocxl_mmio_map safu\n");
	   goto done;
    }

    rc = ocxl_afu_irq_alloc(safu_h, NULL, &irq_h);
    irq_id = ocxl_afu_irq_get_handle(safu_h, irq_h);

    printf("Set irq id source ea field = 0x%016lx\n", (uint64_t)irq_id);
    printf("Attempt interrupt command\n");
    config_param.context = 1;
    config_param.command = AFU_CMD_INTRP_REQ;
    config_param.mem_base_address = (uint64_t)irq_id;
    rc = config_enable_and_run_machine(safu_h, pp_mmio_h, &machine_config, config_param, DIRECTED);
    if(rc != -1) {
	   printf("Response = 0x%x\n", rc);
       printf("safu config_enable_and_run_machine PASS\n");
    }
    else {
	printf("FAILED: config_enable_and_run_machine\n");
	goto done;
    }
    printf("set status data = 0xff\n");
    status[0] = 0xff;
    printf("Polling interrupt completion status\n");
    while(status[0] != 0x0) {
	   nanosleep(&t, &t);
	   //printf("Polling interrupt completion status\n");
    }
    // clear machine config
    rc = clear_machine_config(pp_mmio_h, &machine_config, config_param, DIRECTED);
    if(rc != 0) {
	printf("Failed to clear machine config\n");
	goto done;
    }

    rc = ocxl_afu_event_check(safu_h, NULL, &event, 1);
    printf("Returned from ocxl_read_event -> there is an interrupt\n");
    if(rc == 0) {
	   printf("Error retrieving interrupt event %d\n", rc);
	   return -1;
    }
    //ocxl_mmio_write64(safu_h, ProcessControl_REGISTER, PROCESS_CONTROL_RESTART);
    ocxl_mmio_write64(pp_mmio_h, WED_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, (uint64_t)work_element_descriptor);
    // set test status to completion
    printf("send test completing status.  set status data to 0x55\n");
    status[0] = 0x55;
    printf("Polling test completion status\n");
    while(status[0] != 0x0) {
	   nanosleep(&t, &t);
	   //printf("Polling test completion status = 0x%x\n", status[0]);
    }
    printf("test is completed\n");
done:
    // free device
    printf("Freeing safu device ... \n");
    ocxl_afu_close(safu_h);
    printf("Freeing mafu device ...\n");
    ocxl_afu_close(mafu_h);

    return 0;
}