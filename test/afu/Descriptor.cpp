#include "Descriptor.h"

#include <limits.h>
#include <string>
#include <stdlib.h>
#include <stdint.h>
#include <fstream>
#include <sstream>

using std::string;
using std::ifstream;
using std::stringstream;

Descriptor::Descriptor (string filename):vsec(0x600), port(0x1000), afu_desc(0x300), regs (DESCRIPTOR_NUM_REGS), mmio(0x4000)
{
    info_msg ("Descriptor: Reading descriptor %s file", filename.c_str ());
    parse_descriptor_file (filename);
    info_msg ("Descriptor: afu descriptor successfully initialized");
}

// Parse descriptor file
void
Descriptor::parse_descriptor_file (string filename)
{
    ifstream file (filename.c_str (), std::ifstream::out);

    if (!file.is_open ())
        error_msg
        ("Descriptor::parse_descriptor_file: failed to open file %s",
         filename.c_str ());
    string line, field, colon, s_value, s_data;

    while (getline (file, line)) {
        // skip comments and empty lines
        if (line[0] == '#' || line == "")
            continue;
        stringstream ss (line);

        ss >> field >> colon >> s_value;

        uint64_t value, data;
	uint32_t vsec_offset, vsec_data;
	
	 // re-output s_value as unsigned int
        if (s_value.substr (0, 2) == "0x") {
            stringstream temp (s_value.substr (2));
            temp >> std::hex >> value;
            info_msg ("Descriptor: setting %s with value 0x%x", field.c_str (),
                      value);
        }
        else {
            stringstream temp (s_value);
            temp >> value;
            info_msg ("Descriptor: setting %s with value %d", field.c_str (),
                      value);
        }

	// Test for data and afu_desc fields
        if (field == "data" || field == "afu_desc") {
            if (s_value.substr (0, 2) == "0x")
                s_value.erase(0, 2);
            getline (file, s_data);
            if (s_data.substr (0, 2) == "0x")
                s_data.erase(0, 2);
            value = strtoull(s_value.c_str(), NULL, 16);
            data = strtoull(s_data.c_str(), NULL, 16);
            info_msg ("Descriptor: setting offset 0x%x with value 0x%016llx",
                      value, data);
	    if(field == "data") {
            	//uint64_t offset = to_vector_index(value);
            	//while (offset >= regs.size())
                //    regs.push_back(0);
            	//regs[offset] = data;
		port[value & 0x00000FFF] = data;
            	continue;
	    }
	    if(field == "afu_desc") {
		afu_desc[value] = data;
	    	continue;
	    }
        }
	else {	// default vsec reg values
	    //vsec_offset = strtoul(field.c_str(), NULL, 16);
	    //vsec_data   = strtoul(s_value.c_str(), NULL, 16);
	    if(vsec_offset < 0x600) {
		vsec_offset = strtoul(field.c_str(), NULL, 16);
		vsec_data = strtoul(s_value.c_str(), NULL, 16);
	    	vsec[vsec_offset] = vsec_data;
		//printf("vsec offset = 0x%x vsec data = 0x%08x\n", vsec_offset, vsec[vsec_offset]);
	    } 
	    else if(vsec_offset >0xFFF) {
		port[vsec_offset & 0x00000FFF] = vsec_data;
		//printf("port offset = 0x%x port data = 0x%08x\n", vsec_offset, vsec_data);
	    }  
	}
    }	// end while loop
}

uint32_t Descriptor::to_vector_index (uint32_t byte_address) const
{
    return byte_address >> 3;
}

uint64_t
Descriptor::get_reg (uint32_t word_address, uint32_t mmio_double) const
{
    uint64_t
    data = regs[to_vector_index (word_address << 2)];

    if (mmio_double)
        return
            data;

    if (word_address & 0x1)
        return (data & 0xFFFFFFFF) | ((data & 0xFFFFFFFF) << 32);
    else
        return (data & 0xFFFFFFFF00000000LL) | (data >> 32);
}

bool
Descriptor::is_dedicated () const
{
    return ((get_reg_prog_model () & MASK_IS_DEDICATED) == MASK_IS_DEDICATED);
}

bool
Descriptor::is_directed () const
{
    return ((get_reg_prog_model () == MASK_IS_DIRECTED) == MASK_IS_DIRECTED);
}

// reg0x00
uint16_t
Descriptor::get_num_ints_per_process () const
{
    return (uint16_t) ((regs[to_vector_index (0x00)] >> 48) & 0xFFFF);
}

uint16_t
Descriptor::get_num_of_process () const
{
    return (uint16_t) ((regs[to_vector_index (0x00)] >> 32) & 0xFFFF);
}

uint16_t
Descriptor::get_num_of_afu_CRs () const
{
    return (uint16_t) ((regs[to_vector_index (0x00)] >> 16) & 0xFFFF);
}

uint16_t
Descriptor::get_reg_prog_model () const
{
    return (uint16_t) (regs[to_vector_index (0x00)] & 0xFFFF);
}

// reg0x20
uint64_t
Descriptor::get_AFU_CR_len () const
{
    return regs[to_vector_index (0x20)] & 0xFFFFFFFFFFFFFF;
}

// reg0x28
uint64_t
Descriptor::get_AFU_CR_offset () const
{
    return regs[to_vector_index (0x28)];
}

// reg0x30
uint8_t
Descriptor::get_PerProcessPSA_control () const
{
    return (uint8_t) ((regs[to_vector_index (0x30)] >> 56) & 0xFF);
}

uint64_t
Descriptor::get_PerProcessPSA_length () const
{
    return regs[to_vector_index (0x30)] & 0xFFFFFFFFFFFFFF;
}

// reg0x38
uint64_t
Descriptor::get_PerProcessPSA_offset () const
{
    return regs[to_vector_index (0x38)];
}

// reg0x40
uint64_t
Descriptor::get_AFU_EB_len () const
{
    return regs[to_vector_index (0x40)] & 0xFFFFFFFFFFFFFF;
}

// reg0x48
uint64_t
Descriptor::get_AFU_EB_offset () const
{
    return regs[to_vector_index (0x48)];
}

// vsec registers
uint32_t
Descriptor::get_vsec_reg(uint32_t vsec_offset)
{
    uint32_t vsec_data, offset;

    offset = vsec_offset & 0x0000FFFC;
    if(offset == 0x410) {	// read afu desc reg
	vsec_data = afu_desc[offset];
    }
    else if(offset < 0x1000) {	// read vsec reg
    	vsec_data = vsec[offset];
    }
    //else if (offset >= 0x1000) {
//	vsec_data = port[offset];
//    }

    return vsec_data;
}

void
Descriptor::set_vsec_reg(uint32_t vsec_offset, uint32_t vsec_data)
{
    if(vsec_offset < 0x1000) {	// write to vsec reg
        vsec[vsec_offset] = vsec_data;
    } 
    else if(vsec_offset > 0x1000) {
	set_port_reg(vsec_offset, vsec_data);
    } 
}

uint64_t
Descriptor::get_port_reg(uint32_t offset)
{
    offset = offset & 0x00000FFC;
    return port[offset];
}

void
Descriptor::set_port_reg(uint32_t offset, uint64_t data)
{
    offset = offset & 0x00000FFC;
    port[offset] = data;
}

uint32_t
Descriptor::get_afu_desc_reg(uint32_t offset)
{
    offset = offset & 0x00000FFC;
    return afu_desc[offset];
}

void
Descriptor::set_afu_desc_reg(uint32_t offset, uint32_t data)
{
    offset = offset & 0x00000FFC;
    afu_desc[offset] = data;
}

// mmio memory space 
// 0x0000 - 0x4000
void
Descriptor::set_mmio_mem(uint32_t offset, char *data, uint8_t size)
{
    uint8_t i;
    debug_msg("Descriptor:set_mmio_mem");
    if(offset > 0x4000) {
	error_msg("Descriptor:set_mmio memory out of range");
    }
    //memcpy(&mmio[offset], &data, size);
    for(i=0; i<size; i++)
	mmio[offset+i] = data[i];
    printf("MMIO offset = 0x%x data = 0x", offset);
    for(i=0; i< size; i++)
	printf("%02x", mmio[offset+i]);
    printf("\n");
    //debug_msg("Descriptor:set_mmio_mem: exit");
}

void
Descriptor::get_mmio_mem(uint32_t offset, char *data, uint8_t size)
{
    uint8_t i;
    debug_msg("Descriptor:get_mmio_mem");
    if(offset > 0x4000) {
	error_msg("Descriptor:get_mmio address out of range\n");
    }
    //memcpy(&data, &mmio[offset], size);
    for(i=0; i<size; i++)
	data[i] = mmio[offset+i];
    printf("MMIO offset = 0x%x data = 0x", offset);
    for(i=0; i<size; i++)
	printf("%02x",data[i]);
    printf("\n");
    //debug_msg("Descriptor:get_mmio_mem: exit");
}

