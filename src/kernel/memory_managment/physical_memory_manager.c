#include "physical_memory_manager.h"
#include <stdbigos/math.h>
#include "kernel_config.h"
static phys_addr_t s_ram_start = 0;
static phys_addr_t s_ram_size = 0;



// subpage_availability_bitmap[i][j] describes which frames of size i contain any available frames of size j
static u64* subpage_availability_bitmap[PAGE_SIZE_AMOUNT][PAGE_SIZE_AMOUNT] = {nullptr};
// subpage_availability_bitmap[i][j] describes which frames of size i contain how many frames of size j
static u64* subpage_count[PAGE_SIZE_AMOUNT][PAGE_SIZE_AMOUNT] = {nullptr};
// page_frame_amount[i] describes how many frames of size i fit in RAM
static u64 page_frame_amount[PAGE_SIZE_AMOUNT] = {0};
static u64 topmost_layer = 0;

u16 clz (u64 x){
	x = x | (x >> 1);
	x = x | (x >> 2);
	x = x | (x >> 4);
	x = x | (x >> 8);
	x = x | (x >> 16);
	x = x | (x >> 32);
	x = ~x;
	u16 count = 0;
    while (x) {
        x &= (x - 1);
        count++;
    }
	return count;
}

void set_bitmap(u64* bitmap, u64 position){
    bitmap[position / 64] |= (1 << (63 - (position % 64)));
}

void clear_bitmap(u64* bitmap, u64 position){
    bitmap[position / 64] &= ~(1 << (63 - (position % 64)));
}

void cleanup_pmm(){
    for(u8 i = 0; i<PAGE_SIZE_AMOUNT; ++i){
	    for(u8 j = 0; j < PAGE_SIZE_AMOUNT; ++j) {
	    	if(subpage_availability_bitmap[i][j]) kfree(subpage_availability_bitmap[i][j]);
	    }
        for(u8 j = 0; j < PAGE_SIZE_AMOUNT; ++j) {
	    	if(subpage_count[i][j]) kfree(subpage_count[i][j]);
	    }
    }
}

error_t phys_mem_init(phys_addr_t ram_start, size_t ram_size, phys_mem_region_t* mgr_regionOUT) {
	s_ram_start = ram_start;
	s_ram_size = ram_size;
	u8 vms = 0;
	buffer_t pt_height_buffer = kernel_config_get(KERCFG_PT_HEIGHT);
	const error_t err = =buffer_read_u8(pt_height_buffer, 0, &vms);
	if(err){return ERR_CRITICAL_INTERNAL_FAILURE}
    topmost_layer = vms + 3;
	for(u8 i = 0; i <= topmost_layer; ++i) {
        //calculate the number of frames of given size in RAM
        u64 frames_count = (ram_size << 18) >> (9 * i); //number of 4kb frames in ram / number of 4kb frames in current frame
        DEBUG_PRINTF("psid: %hhu - %lu/n", i, frames_count);
        page_frame_amount[i] = frames_count;
        error_t err = ERR_NONE;
        const u64 ceil_div_size_64 = (page_frame_amount[i] + 63) >> 6;

		
        for(u8 j = 0; j < i; ++j){
            err = kmalloc(ceil_div_size_64 * sizeof(u64), (void*)&subpage_availability_bitmap[i][j]);
		    memset(subpage_availability_bitmap[i][j], 0, ceil_div_size_64 * sizeof(u64));
            if(err){
                cleanup_pmm();
                return ERR_CRITICAL_INTERNAL_FAILURE;
            }
        }
        err = kmalloc(ceil_div_size_64 * sizeof(u64), (void*)&subpage_availability_bitmap[i][i]);
		memset(subpage_availability_bitmap[i][i], 0xFF, ceil_div_size_64 * sizeof(u64));
        if(err){
            cleanup_pmm();
            return ERR_CRITICAL_INTERNAL_FAILURE;
        }


        if(i > 0){
            for(u8 j=0; j<i; ++j){
                err = kmalloc(frames_count * sizeof(u64), (void*)&subpage_count[i][j]);
		        memset(subpage_count[i][j], 0, frames_count * sizeof(u64));
                if(err){
                    cleanup_pmm();
                    return ERR_CRITICAL_INTERNAL_FAILURE;
                }
            }
        }
	}
	return ERR_NONE;
}

error_t set_memory_region_busy(ppn_t ppn, u64 size_in_bytes){
    u64 number_of_smallest_frames = (size_in_bytes + 4095) % 4096;

    u64 lower = ppn;
    u64 upper = lower + number_of_smallest_frames;
    
    //split up frames to make lower and upper bounds match
    u8 current_layer = topmost_layer;
    u64 lower_index = lower >> (9 * current_layer);
    u64 current_lower = lower % (1 << 9 * current_layer);
    u64 upper_index = upper >> (9 * current_layer);
    u64 current_upper = ((upper_index + 1) << (9 * current_layer)) - 1;

    while(current_lower < lower){
        if((subpage_availability_bitmap[current_layer][current_layer][lower_index/64] >> (63 - lower_index%64) & 1) == 1){
            clear_bitmap(subpage_availability_bitmap[current_layer][current_layer], lower_index);
            subpage_count[current_layer][current_layer - 1][lower_index] = 512;
            for(u8 i = current_layer + 1; i<=topmost_layer; i++){
                subpage_count[i][current_layer][current_lower >> (9 * i)] -= 1;
                subpage_count[i][current_layer-1][current_lower >> (9 * i)] += 512;
            }
        }
        //ugly hack that checks if frame of lower_index is split already
        if(subpage_count[current_layer][current_layer-1][lower_index]>0 || 
            subpage_availability_bitmap[current_layer-1][current_layer-1][(lower_index<<9)/64] == 0){  
            current_layer = current_layer - 1;
            lower_index = lower >> (9 * current_layer);
            current_lower = lower % (1 << 9 * current_layer);
        }
        else{
            return ERR_CRITICAL_INTERNAL_FAILURE;
        }
    }

    current_layer = topmost_layer;
    while(current_upper > upper){
        if((subpage_availability_bitmap[current_layer][current_layer][upper_index/64] >> (63 - upper_index%64) & 1) == 1){
            clear_bitmap(subpage_availability_bitmap[current_layer][current_layer], upper_index);
            subpage_count[current_layer][current_layer - 1][upper_index] = 512;
            for(u8 i = current_layer + 1; i<=topmost_layer; i++){
                subpage_count[i][current_layer][current_upper >> (9 * i)] -= 1;
                subpage_count[i][current_layer-1][current_upper >> (9 * i)] += 512;
            }
        }
        if(subpage_count[current_layer][current_layer-1][upper_index]>0 || 
            subpage_availability_bitmap[current_layer-1][current_layer-1][(upper_index<<9)/64] == 0){  
            current_layer = current_layer - 1;
            upper_index = upper >> (9 * current_layer);
            u64 current_upper = ((upper_index + 1) << (9 * current_layer)) - 1;
        }
        else{
            return ERR_CRITICAL_INTERNAL_FAILURE;
        }
    }

    //go through the range alocating frames and updating predescessors
    u64 current_address = lower;
    u64 current_index = lower;
    current_layer = PAGE_SIZE_4kB;
    while(current_address < upper){
        while((upper - current_address) > 1<<(9*(current_layer+1)) && current_index % 512 == 0){
            current_layer += 1;
            current_index /= 512;
        }
        while((upper - current_address) < 1<<(9*current_layer)){
            current_layer -= 1;
            current_index *= 512;
        }

        set_bitmap(subpage_availability_bitmap[current_layer][current_layer], current_index);
        for(u8 i = current_layer + 1; i<=topmost_layer; i++){
            subpage_count[i][current_layer][current_index>>(9 * (i - current_layer))] -= 1;
        }
    }
    return ERR_NONE;
}

error_t phys_mem_alloc_frame(page_size_t page_size, ppn_t* ppnOUT){
    u8 found_size = 0;
    u8 min_frame = 0;
    u64 found_address = 0;
    bool found = false;
    //find frame to alocate / split
    for(u8 lookup_size = page_size; lookup_size <= topmost_layer; lookup_size++){
        u8 current_layer = topmost_layer;
        u64 index = 0;
        while(index <= (page_frame_amount[current_layer] + 63) >> 6){
            if(subpage_availability_bitmap[current_layer][lookup_size][index]>0){
                found_address = index * 64 + clz(subpage_availability_bitmap[current_layer][lookup_size][index]);
                found_size = current_layer;
                min_frame = lookup_size;
                found = true;
                break;
            }
            else{
                index += 1;
                while(index > (page_frame_amount[current_layer] + 63) >> 6 && current_layer > lookup_size){
                    index = index * 512;
                    current_layer -= 1;
                }
            }
        }
        if(found){
            break;
        }
    }
    if(!found){
        return ERR_PHYSICAL_MEMORY_FULL;
    }

    //found size and found address contain parameters of frame to be split or searched, it doesnt have predescesors
    //min_frame contains size of the smallest available frame, possibly to be split

    u8 current_layer = found_size;
    while (current_layer > min_frame){
        found_address *= 512;
        for(u64 i = 0; i < 8; i++){
            if(subpage_availability_bitmap[current_layer - 1][min_frame][found_address / 64 + i] > 0){
                found_address = found_address + (64 * i) + clz(subpage_availability_bitmap[current_layer - 1][min_frame][found_address / 64 + i]);
                break;
            }
        }
        current_layer -= 1;
    }

    *ppnOUT = found_address << (9 * current_layer);

    for(u8 i = page_size; i <= min_frame; i++){
        u64 current_address = found_address << (9 * (min_frame - i));
        for(u8 j = page_size; j < i; j++){
            set_bitmap(subpage_availability_bitmap[i][j], current_address);
            subpage_count[i][j][current_address] = 511;
        }
        clear_bitmap(subpage_availability_bitmap[i][i], current_address);
    }

    for(u8 i = min_frame + 1; i <= found_size; i++){
        u64 current_address = found_address >> (9 * (i - min_frame));
        for(u8 j = page_size; j < min_frame; j++){
            set_bitmap(subpage_availability_bitmap[i][j], current_address);
            subpage_count[i][j][current_address] += 511;
        }
        subpage_count[i][min_frame][current_address] -= 1;
        if(subpage_count[i][min_frame][current_address] == 0){
            clear_bitmap(subpage_availability_bitmap[i][min_frame], current_address);
        }
    }

    return ERR_NONE;
}

error_t phys_mem_free_frame(ppn_t ppn) {
    u8 current_layer = PAGE_SIZE_4kB;
    u8 merge_chain_length = 0;
    u8 dealocated_frame_size = 0;
    u64 index = 0;

    //find index and size of frame ot be dealocated
    bool found = false;
    while(current_layer <= topmost_layer){
        if(ppn % (1 << (current_layer*9)) != 0){
            return ERR_INVALID_ARGUMENT;
        }
        u64 position = ppn>>(current_layer*9);
        u64 field = position % 64;
        u8 offset = position / 64;
        if(((subpage_availability_bitmap[current_layer][current_layer][field] >> (63 - offset)) & 1) == 0){
            index = position;
            set_bitmap(subpage_availability_bitmap[current_layer][current_layer], position);
            merge_chain_length = 0;
            dealocated_frame_size = current_layer;
            found = true;
            break;
        }
        else{
            current_layer = current_layer + 1;
        }
    }

    if(!found){
        return ERR_INVALID_ARGUMENT;
    }

    //go through predescessors and update them, possibly merging
    current_layer = current_layer + 1;

    while(current_layer <= topmost_layer){
        index = index / 512;
        for(int i = 0; i < merge_chain_length - 1; i++){
            subpage_count[current_layer][dealocated_frame_size + i][index] -= 511;
            if(subpage_count[current_layer][dealocated_frame_size + i][index] == 0){
                clear_bitmap(subpage_availability_bitmap[current_layer][dealocated_frame_size + i], index);
            }
        }
        subpage_count[current_layer][dealocated_frame_size + merge_chain_length][index] += 1;
        if (subpage_count[current_layer][dealocated_frame_size + merge_chain_length][index] == 512 && dealocated_frame_size + merge_chain_length == current_layer - 1){
            merge_chain_length += 1;
            subpage_count[current_layer][dealocated_frame_size + merge_chain_length][index] = 0;
            clear_bitmap(subpage_availability_bitmap[current_layer][dealocated_frame_size + merge_chain_length], index);
        }
    }
	return ERR_NONE;
}

error_t phys_mem_find_free_region(u64 alignment, phys_buffer_t busy_regions, phys_mem_region_t* regionOUT) {
	phys_buffer_t reserved_regions = {0};
	//TODO: Read reserved regions from device tree
	u64 ram_size = 0; //TODO: Read ram size from device tree
	phys_buffer_t unavalible_regions[2] = {reserved_regions, busy_regions};
	phys_addr_t curr_region_start = 0;
	bool overlap = false;
	while(curr_region_start + regionOUT->size < ram_size) {
		for(u32 buff_idx = 0; buff_idx < sizeof(unavalible_regions) / sizeof(unavalible_regions[0]); ++buff_idx) {
			for(size_t reg_idx = 0; reg_idx < unavalible_regions[buff_idx].count; ++reg_idx) {
				phys_addr_t unavalible_region_start = unavalible_regions[buff_idx].regions[reg_idx].start;
				phys_addr_t unavalible_region_end = unavalible_region_start + unavalible_regions[buff_idx].regions[reg_idx].size;
				phys_addr_t curr_region_end = curr_region_start + regionOUT->size;
					if(MAX(curr_region_start, unavalible_region_start) < MIN(curr_region_end, unavalible_region_end)) {
						curr_region_start = unavalible_region_end;
						curr_region_start = (phys_addr_t)align_up((u64)curr_region_start, alignment);
						overlap = true;
						break;
					}
			}
			if(overlap) break;
		}
		if(!overlap) {
			regionOUT->start = curr_region_start;
			return ERR_NONE;
		}
	}
	return ERR_PHYSICAL_MEMORY_FULL;
}

