/****************************************************************************
 *   Copyright (C) 2019-2021 by Gene Cooperman, Rohan Garg, Yao Xu          *
 *   gene@ccs.neu.edu, rohgarg@ccs.neu.edu, xu.yao1@northeastern.edu        *
 *                                                                          *
 *  This file is part of DMTCP.                                             *
 *                                                                          *
 *  DMTCP is free software: you can redistribute it and/or                  *
 *  modify it under the terms of the GNU Lesser General Public License as   *
 *  published by the Free Software Foundation, either version 3 of the      *
 *  License, or (at your option) any later version.                         *
 *                                                                          *
 *  DMTCP is distributed in the hope that it will be useful,                *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *  GNU Lesser General Public License for more details.                     *
 *                                                                          *
 *  You should have received a copy of the GNU Lesser General Public        *
 *  License in the files COPYING and COPYING.LESSER.  If not, see           *
 *  <http://www.gnu.org/licenses/>.                                         *
 ****************************************************************************/

#ifdef SINGLE_CART_REORDER
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "cartesian.h"
#endif

#include <fcntl.h>

#include <signal.h>
#include <sys/personality.h>
#include <sys/mman.h>

#include "mpi_files.h"
#include "mana_header.h"
#include "mpi_plugin.h"
#include "lower_half_api.h"
#include "split_process.h"
#include "p2p_log_replay.h"
#include "p2p_drain_send_recv.h"
#include "record-replay.h"
#include "seq_num.h"
#include "mpi_nextfunc.h"
#include "virtual-ids.h"

#include "config.h"
#include "dmtcp.h"
#include "util.h"
#include "jassert.h"
#include "jfilesystem.h"
#include "protectedfds.h"
#include "procselfmaps.h"

using namespace dmtcp;

/* Global variables */
#ifdef SINGLE_CART_REORDER
extern CartesianProperties g_cartesian_properties;
#endif

extern ManaHeader g_mana_header;
extern std::unordered_map<MPI_File, OpenFileParameters> g_params_map;

int g_numMmaps = 0;
MmapInfo_t *g_list = NULL;
mana_state_t mana_state = UNKNOWN_STATE;

// #define DEBUG

#undef dmtcp_skip_memory_region_ckpting
const VA HIGH_ADDR_START = (VA)0x7fff00000000;

static inline int
regionContains(const void *haystackStart,
               const void *haystackEnd,
               const void *needleStart,
               const void *needleEnd)
{
  return needleStart >= haystackStart && needleEnd <= haystackEnd;
}

EXTERNC int
dmtcp_skip_memory_region_ckpting(const ProcMapsArea *area)
{
  if (strstr(area->name, "/dev/zero") ||
      strstr(area->name, "/dev/kgni") ||
      /* DMTCP SysVIPC plugin should be able to C/R this correctly */
//      strstr(area->name, "/SYSV") ||
      strstr(area->name, "/dev/xpmem") ||
      // strstr(area->name, "xpmem") ||
      // strstr(area->name, "hugetlbfs") ||
      strstr(area->name, "/dev/shm")) {
    JTRACE("Ignoring region")(area->name)((void*)area->addr);
    return 1;
  }
  if (!lh_regions_list) return 0;
  for (int i = 0; i < lh_info.numCoreRegions; i++) {
    void *lhStartAddr = lh_regions_list[i].start_addr;
    if (area->addr == lhStartAddr) {
      JTRACE ("Ignoring LH core region") ((void*)area->addr);
      return 1;
    }
  }
  if (!g_list) return 0;
  for (int i = 0; i < g_numMmaps; i++) {
    void *lhMmapStart = g_list[i].addr;
    void *lhMmapEnd = (VA)g_list[i].addr + g_list[i].len;
    if (!g_list[i].unmapped &&
        regionContains(lhMmapStart, lhMmapEnd, area->addr, area->endAddr)) {
      JTRACE("Ignoring region")
           (area->name)((void*)area->addr)(area->size)
           (lhMmapStart)(lhMmapEnd);
      return 1;
    } else if (!g_list[i].unmapped &&
               regionContains(area->addr, area->endAddr,
                              lhMmapStart, lhMmapEnd)) {
      JTRACE("Unhandled case")
           (area->name)((void*)area->addr)(area->size)
           (lhMmapStart)(lhMmapEnd);
    }
  }
  return 0;
}

// Handler for SIGSEGV: forces the code into an infinite loop for attaching
// GDB and debugging
void
segvfault_handler(int signum, siginfo_t *siginfo, void *context)
{
  int dummy = 0;
  JNOTE("Caught a segmentation fault. Attach gdb to inspect...");
  while (!dummy);
}

// Installs a handler for SIGSEGV; useful for debugging crashes
void
initialize_segv_handler()
{
  static struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_flags = SA_SIGINFO;
  action.sa_sigaction = segvfault_handler;
  sigemptyset(&action.sa_mask);

  JASSERT(sigaction(SIGSEGV, &action, NULL) != -1)
    (JASSERT_ERRNO).Text("Could not set up the segfault handler");
}

// Sets the global 'g_list' pointer to the beginning of the MmapInfo_t array
// in the lower half
static void
getLhMmapList()
{
  getMmappedList_t fnc = (getMmappedList_t)lh_info.getMmappedListFptr;
  if (fnc) {
    g_list = fnc(&g_numMmaps);
  }
  JTRACE("Lower half region info")(g_numMmaps);
  for (int i = 0; i < g_numMmaps; i++) {
    JTRACE("Lh region")(g_list[i].addr)(g_list[i].len)(g_list[i].unmapped);
  }
}

// Sets the lower half's __environ variable to point to upper half's __environ
static void
updateLhEnviron()
{
  updateEnviron_t fnc = (updateEnviron_t)lh_info.updateEnvironFptr;
  fnc(__environ);
}

static void
computeUnionOfCkptImageAddresses()
{
  ProcSelfMaps procSelfMaps; // This will be deleted when out of scope.
  Area area;

  // This code assumes that /proc/sys/kernel/randomize_va_space is 0
  //   or that personality(ADDR_NO_RANDOMIZE) was set.
  char buf;
  JWARNING(Util::readAll("/proc/sys/kernel/randomize_va_space", &buf, 1) != 1 ||
           buf == '0' ||
           (personality(0xffffffff) & ADDR_NO_RANDOMIZE) == 0)
    .Text("FIXME:  Linux process randomization is turned on.\n");

  // Initialize the MPI information to NULL (unknown).
  void *libsStart = NULL;
  void *minLibsStart = NULL;
  void *libsEnd = NULL;
  void *maxLibsEnd = NULL;
  void *highMemStart = NULL;
  void *minHighMemStart = NULL;

  // We discover the last address that the kernel had mapped:
  // ASSUMPTIONS:  Target app is not using mmap
  // ASSUMPTIONS:  Kernel does not go back and fill previos gap after munmap
  // TODO: Add a heuristic-based approach to locate the hole between libs+mmap
  //       and bin+heap.
  libsStart = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  JASSERT(libsStart != MAP_FAILED);
  munmap(libsStart, 4096);

  // Preprocess memory regions as needed.
  while (procSelfMaps.getNextArea(&area)) {
    if (Util::strEndsWith(area.name, ".so")) {
      if (libsStart > area.addr) {
        libsStart = area.addr;
      }
      libsEnd = area.endAddr;
    }

    // Set mtcpHdr->highMemStart if possible.
    // highMemStart not currently used.  We could change the constant logic.
    if (highMemStart == NULL && area.addr > HIGH_ADDR_START) {
      highMemStart = area.addr;
    }
  }

  // Adjust libsStart to make 4GB space.
  libsStart = (void *)((uint64_t)libsStart - 4 * ONEGB);

  string kvdb = "MANA_CKPT_UNION";
  dmtcp_kvdb64(DMTCP_KVDB_MIN, kvdb.c_str(), 0, (int64_t) libsStart);
  dmtcp_kvdb64(DMTCP_KVDB_MAX, kvdb.c_str(), 1, (int64_t) libsEnd);
  dmtcp_kvdb64(DMTCP_KVDB_MIN, kvdb.c_str(), 2, (int64_t) highMemStart);

  dmtcp_global_barrier(kvdb.c_str());

  JASSERT(dmtcp_kvdb64_get(kvdb.c_str(), 0, (int64_t*) &minLibsStart) == 0);
  JASSERT(dmtcp_kvdb64_get(kvdb.c_str(), 1, (int64_t*) &maxLibsEnd) == 0);
  JASSERT(dmtcp_kvdb64_get(kvdb.c_str(), 2, (int64_t*) &minHighMemStart) == 0);

  ostringstream o;

#define HEXSTR(o, x) o << #x << std::hex << x;
  HEXSTR(o, libsStart);
  HEXSTR(o, libsEnd);
  HEXSTR(o, highMemStart);
  HEXSTR(o, minLibsStart);
  HEXSTR(o, maxLibsEnd);
  HEXSTR(o, minHighMemStart);

  JTRACE("Union of memory regions") (o.str());

  string minLibsStartStr = jalib::XToString(minLibsStart);
  string maxLibsEndStr = jalib::XToString(maxLibsEnd);
  string minHighMemStartStr = jalib::XToString(minHighMemStart);

  // Now publish these values to DMTCP ckpt-header.
  dmtcp_add_to_ckpt_header("MANA_MinLibsStart", minLibsStartStr.c_str());
  dmtcp_add_to_ckpt_header("MANA_MaxLibsEnd", maxLibsEndStr.c_str());
  dmtcp_add_to_ckpt_header("MANA_MinHighMemStart", minHighMemStartStr.c_str());
}

const char *
get_mana_header_file_name()
{
  struct stat st;
  const char *ckptDir;
  dmtcp::ostringstream o;

  ckptDir = dmtcp_get_ckpt_dir();
  if (stat(ckptDir, &st) == -1) {
    mkdir(ckptDir, 0700); // Create directory if not already exist
  }
  o << ckptDir << "/header.mana";
  return strdup(o.str().c_str());
}

void
save_mana_header(const char *filename)
{
  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (fd == -1) {
    return;
  }

  write(fd, &g_mana_header.init_flag, sizeof(int));
  close(fd);
}

const char *
get_mpi_file_filename()
{
  struct stat st;
  const char *ckptDir;
  dmtcp::ostringstream o;

  ckptDir = dmtcp_get_ckpt_dir();
  if (stat(ckptDir, &st) == -1) {
    mkdir(ckptDir, 0700); // Create directory if not already exist
  }
  o << ckptDir << "/mpi_files.mana";
  return strdup(o.str().c_str());
}

static void
save_mpi_files(const char *filename)
{
  // No need to save open files if there are no files to save
  if (g_params_map.size() == 0) return;

  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (fd == -1) {
    return;
  }

  for (auto itr = g_params_map.begin(); itr != g_params_map.end(); itr++) {
    // Only the most recent value of these properties needs to be saved
    MPI_File_get_position(itr->first, &itr->second._offset);
    MPI_File_get_atomicity(itr->first, &itr->second._atomicity);
    MPI_File_get_size(itr->first, &itr->second._size);

    // Note that a garbage view may be set by this call if set_view was never
    // called, but that view won't be restored
    MPI_File_get_view(itr->first, &itr->second._viewDisp,
                      &itr->second._viewEType, &itr->second._viewFType,
                      itr->second._datarep);

    // Save both virtual ID and corresponding parameters
    write(fd, &itr->first, sizeof(MPI_File));
    write(fd, &itr->second, sizeof(struct OpenFileParameters));
  }

  close(fd);
}

static void
restore_mpi_files(const char *filename)
{
  int fd = open(filename, O_RDONLY, 0755);

  // It's fine if the file doesnt exist, this just means there's no files
  // to be restored
  if (fd == -1) {
    return;
  }

  // Restore the g_params_map with each virtual file and its corresponding
  // parameters from the mapping file
  MPI_File file_buf;
  struct OpenFileParameters params_buf;
  g_params_map.clear();
  size_t bytes_read = 0;
  do {
    bytes_read = read(fd, &file_buf, sizeof(MPI_File));
    bytes_read = read(fd, &params_buf, sizeof(OpenFileParameters));
    g_params_map[file_buf] = params_buf;
  } while(bytes_read != 0);
  close(fd);

  // Restore MPI-created files
  for (auto itr = g_params_map.begin(); itr != g_params_map.end(); itr++) {
    // Re-create file with initial parameters
    int retval;
    MPI_File fh;
    MPI_Comm realComm = VIRTUAL_TO_REAL_COMM(itr->second._comm);
    JUMP_TO_LOWER_HALF(lh_info.fsaddr);
    retval = NEXT_FUNC(File_open)(realComm, itr->second._filepath,
                                  itr->second._mode, itr->second._info, &fh);
    RETURN_TO_UPPER_HALF();
    JASSERT(retval == 0).Text("Restoration of MPI_File_open failed");

    // Update virtual mapping with newly created file
    UPDATE_FILE_MAP(itr->first, fh);

    // Restore file characteristics that are universal to the entire file
    // These are characteristics that do not have to be replayed in order,
    // because only the most recent one will affect the behavior of the file

    // Note that the order of restoration is extremely important (the file size)
    // MUST be set before the view or offset
    MPI_File_set_atomicity(fh, itr->second._atomicity);
    MPI_File_set_size(fh, itr->second._size);

    // Only restore view if required
    if (itr->second._viewSet) {
      MPI_File_set_view(fh, itr->second._viewDisp, itr->second._viewEType,
                        itr->second._viewFType, itr->second._datarep,
                        itr->second._viewInfo);
    }
    MPI_File_seek(fh, itr->second._offset, MPI_SEEK_SET);

  }

}

#ifdef SINGLE_CART_REORDER
const char *
get_cartesian_properties_file_name()
{
  struct stat st;
  const char *ckptDir;
  dmtcp::ostringstream o;

  ckptDir = dmtcp_get_ckpt_dir();
  if (stat(ckptDir, &st) == -1) {
    mkdir(ckptDir, 0700); // Create directory if not already exist
  }
  o << ckptDir << "/cartesian.info";
  return strdup(o.str().c_str());
}

void
save_cartesian_properties(const char *filename)
{
  if (g_cartesian_properties.comm_old_size == -1 ||
      g_cartesian_properties.comm_cart_size == -1 ||
      g_cartesian_properties.comm_old_rank == -1 ||
      g_cartesian_properties.comm_cart_rank == -1) {
    return;
  }
  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (fd == -1) {
    return;
  }
  write(fd, &g_cartesian_properties.comm_old_size, sizeof(int));
  write(fd, &g_cartesian_properties.comm_cart_size, sizeof(int));
  write(fd, &g_cartesian_properties.comm_old_rank, sizeof(int));
  write(fd, &g_cartesian_properties.comm_cart_rank, sizeof(int));
  write(fd, &g_cartesian_properties.reorder, sizeof(int));
  write(fd, &g_cartesian_properties.ndims, sizeof(int));
  int array_size = sizeof(int) * g_cartesian_properties.ndims;
  write(fd, g_cartesian_properties.coordinates, array_size);
  write(fd, g_cartesian_properties.dimensions, array_size);
  write(fd, g_cartesian_properties.periods, array_size);
  close(fd);
}
#endif

static void
mpi_plugin_event_hook(DmtcpEvent_t event, DmtcpEventData_t *data)
{
  switch (event) {
    case DMTCP_EVENT_INIT: {
      JTRACE("*** DMTCP_EVENT_INIT");
      initialize_segv_handler();
      JASSERT(!splitProcess()).Text("Failed to create, initialize lower haf");
      seq_num_init();
      mana_state = RUNNING;
      break;
    }
    case DMTCP_EVENT_EXIT: {
      JTRACE("*** DMTCP_EVENT_EXIT");
      seq_num_destroy();
      break;
    }
    case DMTCP_EVENT_PRESUSPEND: {
      mana_state = CKPT_COLLECTIVE;
      // preSuspendBarrier() will send coord response and get worker state.
      // FIXME:  See commant at: dmtcpplugin.cpp:'case DMTCP_EVENT_PRESUSPEND'
      {
        query_t coord_response = INTENT;
        int64_t round = 0;
        while (1) {
          // FIXME: see informCoordinator...() for the 2pc_data that we send
          //       to the coordinator.  Now, return it and use it below.
          rank_state_t data_to_coord = preSuspendBarrier(coord_response);
          coord_response = Q_UNKNOWN;

          string barrierId = "MANA-PRESUSPEND-" + jalib::XToString(round);
          string csId = "MANA-PRESUSPEND-CS-" + jalib::XToString(round);
          string commId = "MANA-PRESUSPEND-COMM-" + jalib::XToString(round);
          string targetId = "MANA-PRESUSPEND-TARGET-" + jalib::XToString(round);
          int64_t commKey = (int64_t) data_to_coord.comm;

          int target_reached = check_seq_nums();
          if (!target_reached) {
            dmtcp_kvdb64(DMTCP_KVDB_OR, targetId.c_str(), 0, 1);
            coord_response = WAIT_STRAGGLER;
          }

          if (data_to_coord.st == IN_CS) {
            dmtcp_kvdb64(DMTCP_KVDB_INCRBY, csId.c_str(), 0, 1);
            dmtcp_kvdb64(DMTCP_KVDB_OR, commId.c_str(), commKey, 1);
            coord_response = WAIT_STRAGGLER;
          }

          dmtcp_global_barrier(barrierId.c_str());

          int64_t counter;
          // If the database name and key combination does not exist, the in-out
          // parameter will not be changed. So we need to initialize the
          // variable not_all_targets_reached.
          int64_t not_all_targets_reached = 0;
          dmtcp_kvdb64_get(targetId.c_str(), 0, &not_all_targets_reached);

          if (not_all_targets_reached && data_to_coord.st == STOP_BEFORE_CS) {
            coord_response = FREE_PASS;
          }

          // No rank published IN_CS state and all sequence numbers
          // reached the target number.
          if (dmtcp_kvdb64_get(csId.c_str(), 0, &counter) == -1 &&
              !not_all_targets_reached) {
            break;
          }

          if (coord_response == Q_UNKNOWN) {
            int64_t commStatus = 0;
            dmtcp_kvdb64_get(commId.c_str(), commKey, &commStatus);
            if (commStatus == 1 && data_to_coord.st == STOP_BEFORE_CS) {
              coord_response = FREE_PASS;
            } else {
              coord_response = WAIT_STRAGGLER;
            }
          }
          round++;
        }
      }
      break;
    }

    case DMTCP_EVENT_PRECHECKPOINT: {
      dmtcp_local_barrier("MPI:GetLocalLhMmapList");
      getLhMmapList();
      dmtcp_local_barrier("MPI:GetLocalRankInfo");
      getLocalRankInfo(); // p2p_log_replay.cpp
      dmtcp_global_barrier("MPI:update-ckpt-dir-by-rank");
      updateCkptDirByRank(); // mpi_plugin.cpp
      dmtcp_global_barrier("MPI:Register-local-sends-and-receives");
      mana_state = CKPT_P2P;
      registerLocalSendsAndRecvs(); // p2p_drain_send_recv.cpp
      dmtcp_global_barrier("MPI:Drain-Send-Recv");
      drainSendRecv(); // p2p_drain_send_recv.cpp
      computeUnionOfCkptImageAddresses();
      dmtcp_global_barrier("MPI:save-mana-header-and-mpi-files");
      const char *file = get_mana_header_file_name();
      save_mana_header(file);
      const char *file2 = get_mpi_file_filename();
      save_mpi_files(file2);
#ifdef SINGLE_CART_REORDER
      dmtcp_global_barrier("MPI:save-cartesian-properties");
      const char *file = get_cartesian_properties_file_name();
      save_cartesian_properties(file);
#endif
      break;
    }

    case DMTCP_EVENT_RESUME: {
      dmtcp_local_barrier("MPI:Reset-Drain-Send-Recv-Counters");
      resetDrainCounters(); // p2p_drain_send_recv.cpp
      seq_num_reset(RESUME);
      dmtcp_local_barrier("MPI:seq_num_reset");
      mana_state = RUNNING;
      break;
    }

    case DMTCP_EVENT_RESTART: {
      dmtcp_local_barrier("MPI:updateEnviron");
      updateLhEnviron(); // mpi-plugin.cpp
      dmtcp_local_barrier("MPI:Reset-Drain-Send-Recv-Counters");
      resetDrainCounters(); // p2p_drain_send_recv.cpp
      mana_state = RESTART_REPLAY;
#ifdef SINGLE_CART_REORDER
      dmtcp_global_barrier("MPI:setCartesianCommunicator");
      // record-replay.cpp
      setCartesianCommunicator(lh_info.getCartesianCommunicatorFptr);
#endif
      dmtcp_global_barrier("MPI:restoreMpiLogState");
      restoreMpiLogState(); // record-replay.cpp
      dmtcp_global_barrier("MPI:record-replay.cpp-void");
      replayMpiP2pOnRestart(); // p2p_log_replay.cpp
      dmtcp_local_barrier("MPI:p2p_log_replay.cpp-void");
      seq_num_reset(RESTART);
      dmtcp_local_barrier("MPI:seq_num_reset");
      const char *file = get_mpi_file_filename();
      restore_mpi_files(file);
      dmtcp_local_barrier("MPI:Restore-MPI-Files");
      mana_state = RUNNING;
      break;
    }
    default: {
      break;
    }
  }
}

DmtcpPluginDescriptor_t mpi_plugin = {
  DMTCP_PLUGIN_API_VERSION,
  PACKAGE_VERSION,
  "mpi_plugin",
  "DMTCP",
  "dmtcp@ccs.neu.edu",
  "MPI Proxy Plugin",
  mpi_plugin_event_hook
};

DMTCP_DECL_PLUGIN(mpi_plugin);
