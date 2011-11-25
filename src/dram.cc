/**********************************************************************************************
 * File         : dram.cc 
 * Author       : Jaekyu Lee
 * Date         : 11/3/2009
 * SVN          : $Id: dram.cc 912 2009-11-20 19:09:21Z kacear $
 * Description  : Dram Controller 
 *********************************************************************************************/


#include "assert_macros.h"
#include "debug_macros.h"
#include "dram.h"
#include "memory.h"
#include "memreq_info.h"
#include "noc.h"
#include "utils.h"
#include "trace_read.h"

#include "all_knobs.h"
#include "statistics.h"

#include "manifold/kernel/include/kernel/common-defs.h"
#include "manifold/kernel/include/kernel/component-decl.h"
#include "manifold/kernel/include/kernel/clock.h"
#include "manifold/kernel/include/kernel/manifold.h"
#include "manifold/models/iris/iris_srcs/components/manifoldProcessor.h"


#define DEBUG(args...) _DEBUG(*m_simBase->m_knobs->KNOB_DEBUG_DRAM, ## args)

static int total_dram_bandwidth = 0;

///
/// \todo replace g_memory with m_memory
///


///////////////////////////////////////////////////////////////////////////////////////////////
// wrapper functions to allocate dram controller object

dram_controller_c* fcfs_controller(macsim_c* simBase)
{
  dram_controller_c* fcfs = new dram_controller_c(simBase);
  return fcfs;
}


dram_controller_c* frfcfs_controller(macsim_c* simBase)
{
  dram_controller_c* frfcfs = new dc_frfcfs_c(simBase);
  return frfcfs;
}



///////////////////////////////////////////////////////////////////////////////////////////////


int dram_controller_c::dram_req_priority[DRAM_REQ_PRIORITY_COUNT] = 
{
  0, // MRT_IFETCH		
  0, // MRT_DFETCH
  0, // MRT_DSTORE
  0, // MRT_IPRF
  0, // MRT_DPRF
  0, // MRT_WB
  0, // MRT_SW_DPRF
  0, // MRT_SW_DPRF_NTA
  0, // MRT_SW_DPRF_T0
  0, // MRT_SW_DPRF_T1
  0, // MRT_SW_DPRF_T2
  0  // MAX_MEM_REQ_TYPE
};


const char* dram_controller_c::dram_state[DRAM_STATE_COUNT] = {
  "DRAM_INIT",
  "DRAM_CMD",
  "DRAM_CMD_WAIT",
  "DRAM_DATA",
  "DRAM_DATA_WAIT"
};


///////////////////////////////////////////////////////////////////////////////////////////////


int drb_entry_s::m_unique_id = 0;


// drb_entry_s constructor
drb_entry_s::drb_entry_s(macsim_c* simBase)
{
  reset();
  m_simBase = simBase;
}


// reset a drb entry.
void drb_entry_s::reset()
{
  m_id        = -1;
  m_state     = DRAM_INIT;
  m_addr      = 0;
  m_bid       = -1;
  m_rid       = -1;
  m_cid       = -1;
  m_core_id   = -1;
  m_thread_id = -1;
  m_appl_id   = -1;
  m_read      = false;
  m_req       = NULL;
  m_priority  = 0;
  m_size      = 0;
  m_timestamp = 0;
  m_scheduled = 0;
}


// set a drb entry.
void drb_entry_s::set(mem_req_s *mem_req, int bid, int rid, int cid)
{
  m_id        = m_unique_id++;
  m_addr      = mem_req->m_addr;
  m_bid       = bid;
  m_rid       = rid;
  m_cid       = cid;
  m_core_id   = mem_req->m_core_id;
  m_thread_id = mem_req->m_thread_id;
  m_appl_id   = mem_req->m_appl_id;
  m_req       = mem_req;
  m_size      = mem_req->m_size;
  m_timestamp = m_simBase->m_simulation_cycle;
  m_priority  = dram_controller_c::dram_req_priority[mem_req->m_type];

  switch (mem_req->m_type) {
    //case MRT_DSTORE:
    case MRT_WB:
      m_read = false;
      break;
    default:
      m_read = true;
  }

  ASSERT(m_rid >= 0);
}


///////////////////////////////////////////////////////////////////////////////////////////////
// dram controller


// dram controller constructor
dram_controller_c::dram_controller_c(macsim_c* simBase)
{
  m_simBase = simBase;

  m_num_bank             = *m_simBase->m_knobs->KNOB_DRAM_NUM_BANKS;
  m_num_channel          = *m_simBase->m_knobs->KNOB_DRAM_NUM_CHANNEL;
  m_num_bank_per_channel = m_num_bank / m_num_channel;
  m_bus_width            = *m_simBase->m_knobs->KNOB_DRAM_BUS_WIDTH * *m_simBase->m_knobs->KNOB_DRAM_DDR_FACTOR; 
  
  // bank
  m_buffer           = new list<drb_entry_s*>[m_num_bank];
  m_buffer_free_list = new list<drb_entry_s*>[m_num_bank];
  m_current_list     = new drb_entry_s*[m_num_bank];
  m_current_rid      = new int[m_num_bank];
  m_data_ready       = new Counter[m_num_bank];
  m_data_avail       = new Counter[m_num_bank];
  m_bank_ready       = new Counter[m_num_bank];
  m_bank_timestamp   = new Counter[m_num_bank];

  for (int ii = 0; ii < m_num_bank; ++ii) {
    for (int jj = 0; jj < *m_simBase->m_knobs->KNOB_DRAM_BUFFER_SIZE; ++jj) {
      drb_entry_s* new_entry = new drb_entry_s(m_simBase);
      m_buffer_free_list[ii].push_back(new_entry);
    }

    m_data_ready[ii]     = ULLONG_MAX;
    m_data_avail[ii]     = ULLONG_MAX;
    m_bank_ready[ii]     = ULLONG_MAX;
    m_bank_timestamp[ii] = 0;
    m_current_rid[ii]    = -1;
    m_current_list[ii]   = NULL;
  }


  // channel
  m_byte_avail = new int[m_num_channel];
  m_dbus_ready = new Counter[m_num_channel];

  for (int ii = 0; ii < m_num_channel; ++ii) {
    m_byte_avail[ii] = m_bus_width;
    m_dbus_ready[ii] = 0;
  }

  
  // address parsing
  m_cid_mask  = N_BIT_MASK(log2_int(*m_simBase->m_knobs->KNOB_DRAM_ROWBUFFER_SIZE));
  m_bid_shift = log2_int(*m_simBase->m_knobs->KNOB_DRAM_ROWBUFFER_SIZE);
  m_bid_mask  = N_BIT_MASK(log2_int(*m_simBase->m_knobs->KNOB_DRAM_NUM_BANKS));
  m_rid_shift = log2_int(*m_simBase->m_knobs->KNOB_DRAM_NUM_BANKS);
  m_bid_xor_shift = log2_int(*m_simBase->m_knobs->KNOB_L3_LINE_SIZE) + log2_int(512);
  //m_bid_xor_shift = log2_int(*m_simBase->m_knobs->KNOB_L3_LINE_SIZE) + log2_int(*m_simBase->m_knobs->KNOB_L3_NUM_SET);
//  SIZE/*m_simBase->m_knobs->KNOB_L3_LINE_SIZE/*m_simBase->m_knobs->KNOB_L3_ASSOC); 


  // latency
  m_dram_one_cycle_cpu    = *m_simBase->m_knobs->KNOB_CPU_FREQUENCY / *m_simBase->m_knobs->KNOB_DRAM_FREQUENCY;
  m_precharge_latency_cpu = static_cast<int>(m_dram_one_cycle_cpu * *m_simBase->m_knobs->KNOB_DRAM_PRECHARGE);
  m_activate_latency_cpu  = static_cast<int>(m_dram_one_cycle_cpu * *m_simBase->m_knobs->KNOB_DRAM_ACTIVATE);
  m_column_latency_cpu    = static_cast<int>(m_dram_one_cycle_cpu * *m_simBase->m_knobs->KNOB_DRAM_COLUMN);
  
  m_dram_one_cycle_gpu    = *m_simBase->m_knobs->KNOB_GPU_FREQUENCY / *m_simBase->m_knobs->KNOB_DRAM_FREQUENCY;
  m_precharge_latency_gpu = static_cast<int>(m_dram_one_cycle_gpu * *m_simBase->m_knobs->KNOB_DRAM_PRECHARGE);
  m_activate_latency_gpu  = static_cast<int>(m_dram_one_cycle_gpu * *m_simBase->m_knobs->KNOB_DRAM_ACTIVATE);
  m_column_latency_gpu    = static_cast<int>(m_dram_one_cycle_gpu * *m_simBase->m_knobs->KNOB_DRAM_COLUMN);
}


// dram controller destructor
dram_controller_c::~dram_controller_c()
{
  delete[] m_buffer;
  delete[] m_buffer_free_list;
  delete[] m_current_list;
  delete[] m_current_rid;
  delete[] m_data_ready;
  delete[] m_data_avail;
  delete[] m_bank_ready;
  delete[] m_bank_timestamp;

  temp_out.close();
}


// initialize dram controller
void dram_controller_c::init(int id, int noc_id)
{
  m_id     = id;
  m_noc_id = noc_id;
}


// insert a new request from the memory system
bool dram_controller_c::insert_new_req(mem_req_s* mem_req)
{
  // address parsing
  Addr addr = mem_req->m_addr;
  int bid_xor = (addr >> m_bid_xor_shift) & m_bid_mask; 
  int cid = addr & m_cid_mask;	addr = addr >> m_bid_shift;
  int bid = addr & m_bid_mask; addr = addr >> m_rid_shift;
  int rid = addr;

  ASSERTM(rid >= 0, "addr:%s cid:%d bid:%d rid:%d type:%s\n",    \
          hexstr64s(addr), cid, bid, rid,                        \
          mem_req_c::mem_req_type_name[mem_req->m_type]);
  
  // Permutation-based Interleaving
  if (*m_simBase->m_knobs->KNOB_DRAM_BANK_XOR_INDEX) {
    bid = bid ^ bid_xor;
   }

  // check buffer full
  if (m_buffer_free_list[bid].empty()) {
    flush_prefetch(bid);
  
    if (m_buffer_free_list[bid].empty()) {
      return false;
    }
  }

  // insert a new request to DRB
  insert_req_in_drb(mem_req, bid, rid, cid);
  on_insert(mem_req, bid, rid, cid);

  STAT_EVENT(TOTAL_DRAM);


  ++m_total_req;
  mem_req->m_state = MEM_DRAM_START;

  DEBUG("MC[%d] new_req:%d bid:%d rid:%d cid:%d\n", m_id, mem_req->m_id, bid, rid, cid);

  return true;
}


// When the buffer is full, flush all prefetches.
void dram_controller_c::flush_prefetch(int bid)
{
  list<drb_entry_s*> done_list;
  for (auto I = m_buffer[bid].begin(), E = m_buffer[bid].end(); I != E; ++I) {
    if ((*I)->m_req->m_type == MRT_DPRF) {
      done_list.push_back((*I));
    }
  }

  for (auto I = done_list.begin(), E  = done_list.end(); I != E; ++I) {
    m_simBase->m_memory->free_req((*I)->m_req->m_core_id, (*I)->m_req);
    m_buffer_free_list[bid].push_back((*I));
    m_buffer[bid].remove((*I));
    --m_total_req;
  }
}


// insert a new request to dram request buffer (DRB)
void dram_controller_c::insert_req_in_drb(mem_req_s* mem_req, int bid, int rid, int cid)
{
  drb_entry_s* new_entry = m_buffer_free_list[bid].front();
  m_buffer_free_list[bid].pop_front();

  // set drb_entry
  new_entry->set(mem_req, bid, rid, cid); 

  // insert new drb entry to drb 
  m_buffer[bid].push_back(new_entry);

  STAT_EVENT(POWER_MC_W);
}


// tick a cycle
void dram_controller_c::run_a_cycle()
{
  channel_schedule();
  bank_schedule();

  receive_packet();

  // starvation check
  progress_check();
  for (int ii = 0; ii < m_num_channel; ++ii) {
    // check whether the dram bandwidth has been saturated
    if (avail_data_bus(ii)) {
      STAT_EVENT(DRAM_CHANNEL0_DBUS_IDLE + ii);
    }
  }
  on_run_a_cycle();
}


// starvation checking.
void dram_controller_c::progress_check(void)
{
  // if there are requests, but not serviced, increment counter
  if (m_total_req > 0 && m_num_completed_in_last_cycle == 0)
    ++m_starvation_cycle;
  else
    m_starvation_cycle = 0;

  // if counter exceeds N, raise exception
  if (m_starvation_cycle >= 5000) {
    print_req();
    ASSERT(0);
  }
}


void dram_controller_c::print_req(void)
{
  FILE* fp = fopen("bug_detect_dram.out", "w");

  fprintf(fp, "Current cycle:%llu\n", m_simBase->m_simulation_cycle);
  fprintf(fp, "Total req:%d\n", m_total_req);
  fprintf(fp, "\n");
  fprintf(fp, "Data bus\n");
  for (int ii = 0; ii < m_num_channel; ++ii) {
    fprintf(fp, "DBUS[%d] bus_ready:%llu\n", ii, m_data_ready[ii]);
  }

  fprintf(fp, "\n");
  fprintf(fp, "Each bank\n");
  for (int ii = 0; ii < m_num_bank; ++ii) {
    fprintf(fp, "clist:%-10d scheduled:%llu size:%-5d state:%-15s bank_ready:%llu " \
        "data_ready:%llu data_avail:%llu time:%llu\n", \
        (m_current_list[ii] ? m_current_list[ii]->m_req->m_id : -1), \
        (m_current_list[ii] ? m_current_list[ii]->m_scheduled : 0), \
        (int)m_buffer[ii].size(), \
        (m_current_list[ii] ? dram_state[m_current_list[ii]->m_state] : "NULL"), \
        m_bank_ready[ii], m_data_ready[ii], m_data_avail[ii], m_bank_timestamp[ii]);
  }

  fclose(fp);

//  g_memory->print_mshr();
}


///////////////////////////////////////////////////////////////////////////////////////////////
// dram bank activity


// schedule each bank.
void dram_controller_c::bank_schedule()
{
  bank_schedule_complete();
  bank_schedule_new();
}


// check completed request. 
void dram_controller_c::bank_schedule_complete(void)
{
  m_num_completed_in_last_cycle = 0;
  for (int ii = 0; ii < m_num_bank; ++ii) {
    if (m_current_list[ii] == NULL)
      continue;

    if (m_data_ready[ii] <= m_simBase->m_simulation_cycle) {
      ASSERT(m_current_list[ii]->m_state == DRAM_DATA_WAIT);

      // find same address entries
      bool need_to_stop = false;
      if (*m_simBase->m_knobs->KNOB_DRAM_MERGE_REQUESTS) {
        list<drb_entry_s*> temp_list;
        for (auto I = m_buffer[ii].begin(), E  = m_buffer[ii].end(); I != E; ++I) {
          if ((*I)->m_addr == m_current_list[ii]->m_addr) {
            on_complete(*I);
            if ((*I)->m_req->m_type == MRT_WB) {
              DEBUG("MC[%d] merged_req:%d addr:%s type:%s done\n", \
                  m_id, (*I)->m_req->m_id, hexstr64s((*I)->m_req->m_addr), \
                  mem_req_c::mem_req_type_name[(*I)->m_req->m_type]);
              m_simBase->m_memory->free_req((*I)->m_req->m_core_id, (*I)->m_req);
            }
            else {
              if (send_packet((*I)) == false) {
                need_to_stop = true;
                continue;
              }
              (*I)->m_req->m_state = MEM_DRAM_DONE;
              DEBUG("MC[%d] merged_req:%d addr:%s typs:%s done\n", \
                  m_id, (*I)->m_req->m_id, hexstr64s((*I)->m_req->m_addr), \
                  mem_req_c::mem_req_type_name[(*I)->m_req->m_type]);
            }
            temp_list.push_back((*I));
            m_num_completed_in_last_cycle = m_simBase->m_simulation_cycle;
          }
        }

        for (auto I = temp_list.begin(), E = temp_list.end(); I != E; ++I) {
          (*I)->reset();
          m_buffer_free_list[ii].push_back((*I));
          m_buffer[ii].remove((*I));
          STAT_EVENT(TOTAL_DRAM_MERGE);
          --m_total_req;
        }

        temp_list.clear();
      }

      if (need_to_stop) {
        continue;
      }
      
      STAT_EVENT(DRAM_AVG_LATENCY_BASE);
      STAT_EVENT_N(DRAM_AVG_LATENCY, m_simBase->m_simulation_cycle - m_current_list[ii]->m_timestamp);

      m_avg_latency += m_simBase->m_simulation_cycle - m_current_list[ii]->m_timestamp;
      ++m_avg_latency_base;

      on_complete(m_current_list[ii]);
      // wb request will be retired immediately
      if (m_current_list[ii]->m_req->m_type == MRT_WB) {
        DEBUG("MC[%d] req:%d addr:%s type:%s done\n", 
            m_id, m_current_list[ii]->m_req->m_id, \
            hexstr64s(m_current_list[ii]->m_req->m_addr), \
            mem_req_c::mem_req_type_name[m_current_list[ii]->m_req->m_type]);
        m_simBase->m_memory->free_req(m_current_list[ii]->m_req->m_core_id, m_current_list[ii]->m_req);
      }
      // otherwise, send back to interconnection network
      else {
        if (send_packet(m_current_list[ii]) == false) {
          continue;
        }
        m_current_list[ii]->m_req->m_state = MEM_DRAM_DONE;
        DEBUG("MC[%d] req:%d addr:%s type:%s bank:%d done\n", 
            m_id, m_current_list[ii]->m_req->m_id, \
            hexstr64s(m_current_list[ii]->m_req->m_addr), \
            mem_req_c::mem_req_type_name[m_current_list[ii]->m_req->m_type], ii);
      }

      m_current_list[ii]->reset();
      m_buffer_free_list[ii].push_back(m_current_list[ii]);
      m_current_list[ii] = NULL;
      m_data_ready[ii]   = ULLONG_MAX;
      ++m_num_completed_in_last_cycle;
      --m_total_req;
    }
  }
}


bool dram_controller_c::send_packet(drb_entry_s* dram_req)
{
  dram_req->m_req->m_msg_type = NOC_FILL;
  dram_req->m_req->m_msg_src = m_noc_id;
#ifdef IRIS
  dram_req->m_req->m_msg_src = m_terminal->node_id;
  dram_req->m_req->m_msg_dst = 
    m_simBase->m_memory->get_dst_router_id(MEM_L3, dram_req->m_req->m_cache_id[MEM_L3]);
#endif
  int dst_id = m_simBase->m_memory->get_dst_id(MEM_L3, dram_req->m_req->m_cache_id[MEM_L3]);
  assert(dram_req->m_req->m_msg_src != -1 && dram_req->m_req->m_msg_dst != -1);

#ifndef IRIS
#if 0
    //print out of memory req trace
    const int size = 4;
    static long int noc_id[size];
    static long long int prev_gsc = -1;
    //if(prev_gsc != m_simBase->m_simulation_cycle && ) )
	
    {
	noc_id[m_noc_id%size]++;
    	prev_gsc = m_simBase->m_simulation_cycle;
    	cout << "Sim_Cycle, " << m_simBase->m_simulation_cycle << ", ";
    	for(int i=0; i<size; i++)
    	{
    		cout << noc_id[i] << ", ";
    	}
    	cout << "\n";
    }
#endif
#endif
#ifndef IRIS
  if (!m_simBase->m_noc->insert(m_noc_id, dst_id, NOC_FILL, dram_req->m_req)) {
#else
  if (!m_terminal->send_packet(dram_req->m_req)) {
#endif
    DEBUG("MC[%d] req:%d addr:%s type:%s noc busy\n", 
        m_id, dram_req->m_req->m_id, hexstr64s(dram_req->m_req->m_addr), \
        mem_req_c::mem_req_type_name[dram_req->m_req->m_type]);
    return false;
  }
  return true;
}


void dram_controller_c::receive_packet(void)
{
#ifdef IRIS
  // check router queue every cycle
  if (!m_terminal->receive_queue.empty()) {
    mem_req_s* req = m_terminal->receive_queue.front();
    if (insert_new_req(req)) {
      m_terminal->receive_queue.pop();
    }
  }
#endif
}


// when current list is empty, schedule a new request.
// otherwise, make it ready for next command.
void dram_controller_c::bank_schedule_new(void)
{
  for (int ii = 0; ii < m_num_bank; ++ii) {
    if (m_buffer[ii].empty() && m_current_list[ii] == NULL)
      continue;

    // current list is empty. find a new one.
    if (m_current_list[ii] == NULL) {
      drb_entry_s* entry = schedule(&m_buffer[ii]);
      ASSERT(entry);

      m_current_list[ii] = entry;
      m_current_list[ii]->m_state = DRAM_CMD;
      m_current_list[ii]->m_scheduled = m_simBase->m_simulation_cycle;

      m_buffer[ii].remove(entry);

      m_bank_ready[ii]     = ULLONG_MAX;
      m_bank_timestamp[ii] = m_simBase->m_simulation_cycle;

      STAT_EVENT(POWER_MC_R);

      DEBUG("bank[%d] req:%d has been selected\n", ii, m_current_list[ii]->m_req->m_id);
    }
    // previous command is done. ready for next sequence of command.
    else if (m_bank_ready[ii] <= m_simBase->m_simulation_cycle && 
        m_current_list[ii]->m_state == DRAM_CMD_WAIT) {
      ASSERT(m_current_list[ii]->m_state == DRAM_CMD_WAIT || 
          m_current_list[ii]->m_state == DRAM_DATA);
      m_bank_ready[ii] = ULLONG_MAX;
      m_current_list[ii]->m_state = DRAM_CMD;
      m_bank_timestamp[ii] = m_simBase->m_simulation_cycle;
    }
  }
}


// select highest priority request based on the policy.
drb_entry_s* dram_controller_c::schedule(list<drb_entry_s*> *buffer)
{
  ASSERT(!buffer->empty());

  // FCFS (First Come First Serve)
  drb_entry_s* entry = buffer->front();

  return entry;
}


///////////////////////////////////////////////////////////////////////////////////////////////
// dram channel activity


// schedule dram channels.
void dram_controller_c::channel_schedule(void)
{
  channel_schedule_cmd();
  channel_schedule_data();
}


// schedule command-ready bank.
void dram_controller_c::channel_schedule_cmd(void)
{
  for (int ii = 0; ii < m_num_channel; ++ii) {
    Counter oldest = ULLONG_MAX;
    int bank = -1;
    for (int jj = ii * m_num_bank_per_channel; jj < (ii + 1) * m_num_bank_per_channel; ++jj) {
      if (m_current_list[jj] != NULL && 
          m_current_list[jj]->m_state == DRAM_CMD &&
          m_bank_timestamp[jj] < oldest) {
        oldest = m_bank_timestamp[jj];
        bank   = jj;
      }
    }

    if (bank != -1) {
      ASSERT(m_current_list[bank]->m_state == DRAM_CMD);
      m_current_list[bank]->m_req->m_state = MEM_DRAM_CMD;
      // activate
      if (m_current_rid[bank] == -1) {
        m_current_rid[bank] = m_current_list[bank]->m_rid;
        m_bank_ready[bank]  = m_simBase->m_simulation_cycle + 
          (m_current_list[bank]->m_req->m_ptx ? m_activate_latency_gpu : m_activate_latency_cpu);;
        m_data_avail[bank]   = ULLONG_MAX;
        m_current_list[bank]->m_state = DRAM_CMD_WAIT;
        STAT_EVENT(DRAM_ACTIVATE);
        DEBUG("bank[%d] req:%d activate\n", bank, m_current_list[bank]->m_req->m_id);
      }
      // column access
      else if (m_current_list[bank]->m_rid == m_current_rid[bank]) {
        m_bank_ready[bank] = m_simBase->m_simulation_cycle + 
          (m_current_list[bank]->m_req->m_ptx ? m_column_latency_gpu : m_column_latency_cpu);;
        m_data_avail[bank] = m_bank_ready[bank];
        m_current_list[bank]->m_state = DRAM_DATA;
        STAT_EVENT(DRAM_COLUMN);
        DEBUG("bank[%d] req:%d column\n", bank, m_current_list[bank]->m_req->m_id);
      }
      // precharge
      else {
        m_current_rid[bank] = -1;
        m_bank_ready[bank]  = m_simBase->m_simulation_cycle + 
          (m_current_list[bank]->m_req->m_ptx ? m_precharge_latency_gpu : m_precharge_latency_cpu);;
        m_data_avail[bank]   = ULLONG_MAX;
        m_current_list[bank]->m_state = DRAM_CMD_WAIT;
        STAT_EVENT(DRAM_PRECHARGE);
        DEBUG("bank[%d] req:%d precharge\n", bank, m_current_list[bank]->m_req->m_id);
      }
    }
  }
}


// schedule data-ready bank.
void dram_controller_c::channel_schedule_data(void)
{
  for (int ii = 0; ii < m_num_channel; ++ii) {
    // check whether the dram bandwidth has been saturated
    if (!avail_data_bus(ii)) {
      bool found = false;
      for (int jj = ii * m_num_bank_per_channel; jj < (ii + 1) * m_num_bank_per_channel; ++jj) {
        if (m_current_list[jj] != NULL && 
            m_current_list[jj]->m_state == DRAM_DATA &&
            m_data_avail[jj] <= m_simBase->m_simulation_cycle) {
          found = true;
          break;
        }
      }

      if (found) {
        STAT_EVENT(DRAM_CHANNEL0_BANDWIDTH_SATURATED + ii);
      }
    }
      

    while (avail_data_bus(ii)) {
      Counter oldest = ULLONG_MAX;
      int bank = -1;
      for (int jj = ii * m_num_bank_per_channel; jj < (ii + 1) * m_num_bank_per_channel; ++jj) {
        if (m_current_list[jj] != NULL && 
            m_current_list[jj]->m_state == DRAM_DATA &&
            m_data_avail[jj] <= m_simBase->m_simulation_cycle &&
            m_bank_timestamp[jj] < oldest) {
          oldest = m_bank_timestamp[jj];
          bank = jj;
        }
      }

      if (bank != -1) {
        m_current_list[bank]->m_req->m_state = MEM_DRAM_DATA;
        DEBUG("bank[%d] req:%d has acquired data bus\n", \
            bank, m_current_list[bank]->m_req->m_id);
        ASSERT(m_current_list[bank]->m_state == DRAM_DATA);
        m_data_ready[bank] = acquire_data_bus(ii, m_current_list[bank]->m_size, m_current_list[bank]->m_req->m_ptx);
        m_data_avail[bank] = ULLONG_MAX;
        m_current_list[bank]->m_state = DRAM_DATA_WAIT;
      }
      else
        break;
    }
  }
}


// check data bus availability.
bool dram_controller_c::avail_data_bus(int channel_id)
{
  if (m_dbus_ready[channel_id] <= m_simBase->m_simulation_cycle)
    return true;

  return false;
}


// acquire data bus.
Counter dram_controller_c::acquire_data_bus(int channel_id, int req_size, bool gpu_req)
{
  m_band += req_size;
  total_dram_bandwidth += req_size;
  STAT_EVENT_N(BANDWIDTH_TOT, req_size);
  Counter latency;
  // when the size of a request is less than bus width, we can have more requests per cycle
  if (req_size < m_byte_avail[channel_id]) {
    m_byte_avail[channel_id] -= req_size;
    latency = m_simBase->m_simulation_cycle;
  }
  else {
    int cycle = (req_size - m_byte_avail[channel_id]) / m_bus_width + 1;
    int dram_cycle;
    
    if (gpu_req) 
      dram_cycle = static_cast<int>(cycle * m_dram_one_cycle_gpu + 0.5);
    else
      dram_cycle = static_cast<int>(cycle * m_dram_one_cycle_cpu + 0.5);
    latency = m_simBase->m_simulation_cycle + dram_cycle;
    m_byte_avail[channel_id] = m_bus_width - 
      (req_size - m_byte_avail[channel_id]) % m_bus_width;
  }

  m_dbus_ready[channel_id] = latency;

  return latency;
}


// create the network interface
void dram_controller_c::create_network_interface(void)
{
#ifdef IRIS
  manifold::kernel::CompId_t processor_id = 
    manifold::kernel::Component::Create<ManifoldProcessor>(0, m_simBase);
  m_terminal = manifold::kernel::Component::GetComponent<ManifoldProcessor>(processor_id);
  manifold::kernel::Clock::Register<ManifoldProcessor>(m_terminal, &ManifoldProcessor::tick, 
      &ManifoldProcessor::tock);

  m_terminal->mclass = MC_RESP; //PROC_REQ;//
  m_simBase->m_macsim_terminals.push_back(m_terminal);

  m_noc_id = static_cast<int>(processor_id);
#endif
}

void dram_controller_c::on_insert(mem_req_s* req, int bid, int rid, int cid)
{
  // empty
}

void dram_controller_c::on_complete(drb_entry_s* req)
{
  // empty
}

void dram_controller_c::on_run_a_cycle()
{
  // empty
}


///////////////////////////////////////////////////////////////////////////////////////////////


dc_frfcfs_c::sort_func::sort_func(dc_frfcfs_c *parent)
{
  m_parent = parent;
}


bool dc_frfcfs_c::sort_func::operator()(const drb_entry_s* req_a, const drb_entry_s* req_b)
{
  int bid = req_a->m_bid;
  int current_rid = m_parent->m_current_rid[bid];

  if (req_a->m_req->m_type != MRT_DPRF && req_b->m_req->m_type == MRT_DPRF)
    return true;
  
  if (req_a->m_req->m_type == MRT_DPRF && req_b->m_req->m_type != MRT_DPRF)
    return false;

  if (req_a->m_rid == current_rid && req_b->m_rid != current_rid)
    return true;

  if (req_a->m_rid != current_rid && req_b->m_rid == current_rid)
    return false;

  return req_a->m_timestamp < req_b->m_timestamp;
}


dc_frfcfs_c::dc_frfcfs_c(macsim_c* simBase): dram_controller_c(simBase) {
  m_sort = new sort_func(this);
}


dc_frfcfs_c::~dc_frfcfs_c()
{
}


drb_entry_s* dc_frfcfs_c::schedule(list<drb_entry_s*>* buffer)
{
  buffer->sort(*m_sort);

  return buffer->front();
}



