#ifndef _AMPIIMPL_H
#define _AMPIIMPL_H

#include <string.h> /* for strlen */
#include <algorithm>
#include <numeric>
#include <forward_list>
#include <bitset>

#include "ampi.h"
#include "ddt.h"
#include "charm++.h"

using std::vector;

//Uncomment for debug print statements
#define AMPI_DEBUG(...) //CkPrintf(__VA_ARGS__)

/*
 * All MPI_* routines must be defined using the AMPI_API_IMPL macro.
 * All calls inside AMPI to MPI_* routines must use MPI_* as the name.
 * There are two reasons for this:
 *
 * 1. AMPI supports the PMPI interface only on Linux.
 *
 * 2. When AMPI is built on top of MPI, we rename the user's MPI_* calls as AMPI_*.
 */
#define STRINGIFY_INTERNAL(a) #a
#define STRINGIFY(a) STRINGIFY_INTERNAL(a)

// keep in sync with ampi_noimpl.C
#if AMPI_HAVE_PMPI
  #define AMPI_API_IMPL(ret, name, ...) \
    CLINKAGE \
    __attribute__((weak, alias(STRINGIFY(name)))) \
    ret P##name(__VA_ARGS__); \
    CLINKAGE \
    __attribute__((weak)) \
    ret name(__VA_ARGS__)
#else // not Linux (no PMPI support):
  #define AMPI_API_IMPL(ret, name, ...) \
    CLINKAGE \
    ret name(__VA_ARGS__)
#endif

extern char * ampi_binary_path;

#if AMPIMSGLOG
#include "ckliststring.h"
static CkListString msgLogRanks;
static int msgLogWrite;
static int msgLogRead;
static char *msgLogFilename;

#if CMK_USE_ZLIB && 0
#include <zlib.h>
namespace PUP{
class zdisk : public er {
 protected:
  gzFile F;//Disk file to read from/write to
  zdisk(unsigned int type,gzFile f):er(type),F(f) {}
  zdisk(const zdisk &p);			//You don't want to copy
  void operator=(const zdisk &p);	// You don't want to copy

  //For seeking (pack/unpack in different orders)
  virtual void impl_startSeek(seekBlock &s); /*Begin a seeking block*/
  virtual int impl_tell(seekBlock &s); /*Give the current offset*/
  virtual void impl_seek(seekBlock &s,int off); /*Seek to the given offset*/
};

//For packing to a disk file
class tozDisk : public zdisk {
 protected:
  //Generic bottleneck: pack n items of size itemSize from p.
  virtual void bytes(void *p,int n,size_t itemSize,dataType t);
 public:
  //Write data to the given file pointer
  // (must be opened for binary write)
  // You must close the file yourself when done.
  tozDisk(gzFile f):zdisk(IS_PACKING,f) {}
};

//For unpacking from a disk file
class fromzDisk : public zdisk {
 protected:
  //Generic bottleneck: unpack n items of size itemSize from p.
  virtual void bytes(void *p,int n,size_t itemSize,dataType t);
 public:
  //Write data to the given file pointer
  // (must be opened for binary read)
  // You must close the file yourself when done.
  fromzDisk(gzFile f):zdisk(IS_UNPACKING,f) {}
};
}; // namespace PUP
#endif
#endif // AMPIMSGLOG

/* AMPI sends messages inline to PE-local destination VPs if: BigSim is not being used and
 * if tracing is not being used (see bug #1640 for more details on the latter). */
#ifndef AMPI_LOCAL_IMPL
#define AMPI_LOCAL_IMPL ( !CMK_BIGSIM_CHARM && !CMK_TRACE_ENABLED )
#endif

/* messages larger than or equal to this threshold may block on a matching recv if local to the PE*/
#ifndef AMPI_PE_LOCAL_THRESHOLD_DEFAULT
#define AMPI_PE_LOCAL_THRESHOLD_DEFAULT 4096
#endif

/* messages larger than or equal to this threshold may block on a matching recv if local to the Node */
#ifndef AMPI_NODE_LOCAL_THRESHOLD_DEFAULT
#define AMPI_NODE_LOCAL_THRESHOLD_DEFAULT 16384
#endif

/* AMPI uses RDMA sends if BigSim is not being used and the underlying comm
 * layer supports it (except for GNI, which has experimental RDMA support). */
#ifndef AMPI_RDMA_IMPL
#define AMPI_RDMA_IMPL ( !CMK_BIGSIM_CHARM && CMK_ONESIDED_IMPL && !CMK_CONVERSE_UGNI )
#endif

/* contiguous messages larger than or equal to this threshold are sent via RDMA */
#ifndef AMPI_RDMA_THRESHOLD_DEFAULT
#if CMK_USE_IBVERBS || CMK_OFI || CMK_CONVERSE_UGNI
#define AMPI_RDMA_THRESHOLD_DEFAULT 65536
#else
#define AMPI_RDMA_THRESHOLD_DEFAULT 32768
#endif
#endif

extern int AMPI_RDMA_THRESHOLD;

#define AMPI_ALLTOALL_THROTTLE   64
#define AMPI_ALLTOALL_SHORT_MSG  256
#if CMK_BIGSIM_CHARM
#define AMPI_ALLTOALL_LONG_MSG   4194304
#else
#define AMPI_ALLTOALL_LONG_MSG   32768
#endif

typedef void (*MPI_MigrateFn)(void);

/*
 * AMPI Message Matching (Amm) Interface:
 * messages are matched on 2 ints: [tag, src]
 */
#define AMM_TAG   0
#define AMM_SRC   1
#define AMM_NTAGS 2

// Number of AmmEntry<T>'s in AmmEntryPool for pt2pt msgs:
#ifndef AMPI_AMM_PT2PT_POOL_SIZE
#define AMPI_AMM_PT2PT_POOL_SIZE 32
#endif

// Number of AmmEntry<T>'s in AmmEntryPool for coll msgs:
#ifndef AMPI_AMM_COLL_POOL_SIZE
#define AMPI_AMM_COLL_POOL_SIZE 4
#endif

class AmpiRequestList;

typedef void (*AmmPupMessageFn)(PUP::er& p, void **msg);

template <class T>
class AmmEntry {
 public:
  int tags[AMM_NTAGS]; // [tag, src]
  AmmEntry<T>* next;
  T msg; // T is either an AmpiRequest* or an AmpiMsg*
  AmmEntry(T m) noexcept { tags[AMM_TAG] = m->getTag(); tags[AMM_SRC] = m->getSrcRank(); next = NULL; msg = m; }
  AmmEntry(int tag, int src, T m) noexcept { tags[AMM_TAG] = tag; tags[AMM_SRC] = src; next = NULL; msg = m; }
  AmmEntry() = default;
  ~AmmEntry() = default;
};

template <class T, size_t N>
class Amm {
 public:
  AmmEntry<T>* first;
  AmmEntry<T>** lasth;

 private:
  int startIdx;
  std::bitset<N> validEntries;
  std::array<AmmEntry<T>, N> entryPool;

 public:
  Amm() noexcept : first(NULL), lasth(&first), startIdx(0) { validEntries.reset();  }
  ~Amm() = default;
  inline AmmEntry<T>* newEntry(int tag, int src, T msg) noexcept {
    if (validEntries.all()) {
      return new AmmEntry<T>(tag, src, msg);
    } else {
      for (int i=startIdx; i<validEntries.size(); i++) {
        if (!validEntries[i]) {
          validEntries[i] = 1;
          AmmEntry<T>* ent = new (&entryPool[i]) AmmEntry<T>(tag, src, msg);
          startIdx = i+1;
          return ent;
        }
      }
      CkAbort("AMPI> failed to find a free entry in pool!");
      return NULL;
    }
  }
  inline AmmEntry<T>* newEntry(T msg) noexcept {
    if (validEntries.all()) {
      return new AmmEntry<T>(msg);
    } else {
      for (int i=startIdx; i<validEntries.size(); i++) {
        if (!validEntries[i]) {
          validEntries[i] = 1;
          AmmEntry<T>* ent = new (&entryPool[i]) AmmEntry<T>(msg);
          startIdx = i+1;
          return ent;
        }
      }
      CkAbort("AMPI> failed to find a free entry in pool!");
      return NULL;
    }
  }
  inline void deleteEntry(AmmEntry<T> *ent) noexcept {
    if (ent >= &entryPool.front() && ent <= &entryPool.back()) {
      int idx = (int)((intptr_t)ent - (intptr_t)&entryPool.front()) / sizeof(AmmEntry<T>);
      validEntries[idx] = 0;
      startIdx = std::min(idx, startIdx);
    } else {
      delete ent;
    }
  }
  void freeAll() noexcept;
  void flushMsgs() noexcept;
  inline bool match(const int tags1[AMM_NTAGS], const int tags2[AMM_NTAGS]) const noexcept;
  inline void put(T msg) noexcept;
  inline void put(int tag, int src, T msg) noexcept;
  inline T get(int tag, int src, int* rtags=NULL) noexcept;
  inline T probe(int tag, int src, int* rtags) noexcept;
  inline int size() const noexcept;
  void pup(PUP::er& p, AmmPupMessageFn msgpup) noexcept;
};

PUPfunctionpointer(MPI_User_function*)

/*
 * OpStruct's are used to lookup an MPI_User_function* and check its commutativity.
 * They are also used to create AmpiOpHeader's, which are transmitted in reductions
 * that are user-defined or else lack an equivalent Charm++ reducer type.
 */
class OpStruct {
 public:
  MPI_User_function* func;
  bool isCommutative;
 private:
  bool isValid;

 public:
  OpStruct() = default;
  OpStruct(MPI_User_function* f) noexcept : func(f), isCommutative(true), isValid(true) {}
  OpStruct(MPI_User_function* f, bool c) noexcept : func(f), isCommutative(c), isValid(true) {}
  void init(MPI_User_function* f, bool c) noexcept {
    func = f;
    isCommutative = c;
    isValid = true;
  }
  bool isFree() const noexcept { return !isValid; }
  void free() noexcept { isValid = false; }
  void pup(PUP::er &p) {
    p|func;  p|isCommutative;  p|isValid;
  }
};

class AmpiOpHeader {
 public:
  MPI_User_function* func;
  MPI_Datatype dtype;
  int len;
  int szdata;
  AmpiOpHeader(MPI_User_function* f,MPI_Datatype d,int l,int szd) noexcept :
    func(f),dtype(d),len(l),szdata(szd) { }
};

/*
 * For within-node Ssend's: used in ampi::processSsendMsg()
 */
class SsendInfo {
 private:
  int node;
  int idx;
  int count;
  char* buf;
  CkDDT_DataType* ddt;

 public:
  SsendInfo() = default;
  SsendInfo(int idx_, int count_, char* buf_, CkDDT_DataType* ddt_) noexcept :
    node(CkMyNode()), idx(idx_), count(count_), buf(buf_), ddt(ddt_) {}
  ~SsendInfo() {}
  inline int getNode() const noexcept { return node; }
  inline int getIdx() const noexcept { return idx; }
  inline int getCount() const noexcept { return count; }
  inline char* getBuf() const noexcept { CkAssert(node == CkMyNode()); return buf; }
  inline CkDDT_DataType* getDDT() const noexcept { CkAssert(node == CkMyNode()); return ddt; }
};
PUPbytes(SsendInfo) // PUP as bytes b/c buf & ddt are only meant to be accessed from within shared memory

//------------------- added by YAN for one-sided communication -----------
/* the index is unique within a communicator */
class WinStruct{
 public:
  MPI_Comm comm;
  int index;

private:
  bool areRecvsPosted;
  bool inEpoch;
  vector<int> exposureRankList;
  vector<int> accessRankList;
  vector<MPI_Request> requestList;

public:
  WinStruct() noexcept : comm(MPI_COMM_NULL), index(-1), areRecvsPosted(false), inEpoch(false) {
    exposureRankList.clear(); accessRankList.clear(); requestList.clear();
  }
  WinStruct(MPI_Comm comm_, int index_) noexcept : comm(comm_), index(index_), areRecvsPosted(false), inEpoch(false) {
    exposureRankList.clear(); accessRankList.clear(); requestList.clear();
  }
  void pup(PUP::er &p) noexcept {
    p|comm; p|index; p|areRecvsPosted; p|inEpoch; p|exposureRankList; p|accessRankList; p|requestList;
  }
  void clearEpochAccess() noexcept {
    accessRankList.clear(); inEpoch = false;
  }
  void clearEpochExposure() noexcept {
    exposureRankList.clear(); areRecvsPosted = false; requestList.clear(); inEpoch=false;
  }
  vector<int>& getExposureRankList() noexcept {return exposureRankList;}
  vector<int>& getAccessRankList() noexcept {return accessRankList;}
  void setExposureRankList(vector<int> &tmpExposureRankList) noexcept {exposureRankList = tmpExposureRankList;}
  void setAccessRankList(vector<int> &tmpAccessRankList) noexcept {accessRankList = tmpAccessRankList;}
  vector<int>& getRequestList() noexcept {return requestList;}
  bool AreRecvsPosted() const noexcept {return areRecvsPosted;}
  void setAreRecvsPosted(bool setR) noexcept {areRecvsPosted = setR;}
  bool isInEpoch() const noexcept {return inEpoch;}
  void setInEpoch(bool arg) noexcept {inEpoch = arg;}
};

class lockQueueEntry {
 public:
  int requestRank;
  int lock_type;
  lockQueueEntry (int _requestRank, int _lock_type) noexcept
    : requestRank(_requestRank), lock_type(_lock_type) {}
  lockQueueEntry() = default;
};

typedef CkQ<lockQueueEntry *> LockQueue;

class ampiParent;

class win_obj {
 public:
  void *baseAddr;
  MPI_Aint winSize;
  int disp_unit;
  MPI_Comm comm;

  int owner; // Rank of owner of the lock, -1 if not locked
  LockQueue lockQueue; // queue of waiting processors for the lock
                       // top of queue is the one holding the lock
                       // queue is empty if lock is not applied
  std::string winName;
  bool initflag;

  vector<int> keyvals; // list of keyval attributes

  void setName(const char *src) noexcept;
  void getName(char *src,int *len) noexcept;

 public:
  void pup(PUP::er &p) noexcept;

  win_obj() noexcept;
  win_obj(const char *name, void *base, MPI_Aint size, int disp_unit, MPI_Comm comm) noexcept;
  ~win_obj() noexcept;

  int create(const char *name, void *base, MPI_Aint size, int disp_unit,
             MPI_Comm comm) noexcept;
  int free() noexcept;

  vector<int>& getKeyvals() { return keyvals; }

  int put(void *orgaddr, int orgcnt, int orgunit,
          MPI_Aint targdisp, int targcnt, int targunit) noexcept;

  int get(void *orgaddr, int orgcnt, int orgunit,
          MPI_Aint targdisp, int targcnt, int targunit) noexcept;
  int accumulate(void *orgaddr, int count, MPI_Aint targdisp, MPI_Datatype targtype,
                 MPI_Op op, ampiParent* pptr) noexcept;

  int iget(int orgcnt, MPI_Datatype orgtype,
          MPI_Aint targdisp, int targcnt, MPI_Datatype targtype) noexcept;
  int igetWait(MPI_Request *req, MPI_Status *status) noexcept;
  int igetFree(MPI_Request *req, MPI_Status *status) noexcept;

  int fence() noexcept;

  int lock(int requestRank, int lock_type) noexcept;
  int unlock(int requestRank) noexcept;

  int wait() noexcept;
  int post() noexcept;
  int start() noexcept;
  int complete() noexcept;

  void lockTopQueue() noexcept;
  void enqueue(int requestRank, int lock_type) noexcept;
  void dequeue() noexcept;
  bool emptyQueue() noexcept;
};
//-----------------------End of code by YAN ----------------------

class KeyvalPair{
 protected:
  std::string key;
  std::string val;
 public:
  KeyvalPair() = default;
  KeyvalPair(const char* k, const char* v) noexcept;
  ~KeyvalPair() = default;
  void pup(PUP::er& p) noexcept {
    p|key;
    p|val;
  }
  friend class InfoStruct;
};

class InfoStruct{
  CkPupPtrVec<KeyvalPair> nodes;
  bool valid;
 public:
  InfoStruct() noexcept : valid(true) { }
  void setvalid(bool valid_) noexcept { valid = valid_; }
  bool getvalid() const noexcept { return valid; }
  int set(const char* k, const char* v) noexcept;
  int dup(InfoStruct& src) noexcept;
  int get(const char* k, int vl, char*& v, int *flag) const noexcept;
  int deletek(const char* k) noexcept;
  int get_valuelen(const char* k, int* vl, int *flag) const noexcept;
  int get_nkeys(int *nkeys) const noexcept;
  int get_nthkey(int n,char* k) const noexcept;
  void myfree() noexcept;
  void pup(PUP::er& p) noexcept;
};

class CProxy_ampi;
class CProxyElement_ampi;

//Virtual class describing a virtual topology: Cart, Graph, DistGraph
class ampiTopology {
 private:
  vector<int> v; // dummy variable for const& returns from virtual functions

 public:
  virtual ~ampiTopology() noexcept {};
  virtual void pup(PUP::er &p) noexcept =0;
  virtual int getType() const noexcept =0;
  virtual void dup(ampiTopology* topo) noexcept =0;
  virtual const vector<int> &getnbors() const noexcept =0;
  virtual void setnbors(const vector<int> &nbors_) noexcept =0;

  virtual const vector<int> &getdims() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual const vector<int> &getperiods() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual int getndims() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return -1;}
  virtual void setdims(const vector<int> &dims_) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setperiods(const vector<int> &periods_) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setndims(int ndims_) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}

  virtual int getnvertices() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return -1;}
  virtual const vector<int> &getindex() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual const vector<int> &getedges() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual void setnvertices(int nvertices_) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setindex(const vector<int> &index_) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setedges(const vector<int> &edges_) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}

  virtual int getInDegree() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return -1;}
  virtual const vector<int> &getSources() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual const vector<int> &getSourceWeights() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual int getOutDegree() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return -1;}
  virtual const vector<int> &getDestinations() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual const vector<int> &getDestWeights() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual bool areSourcesWeighted() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return false;}
  virtual bool areDestsWeighted() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return false;}
  virtual void setAreSourcesWeighted(bool val) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setAreDestsWeighted(bool val) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setInDegree(int degree) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setSources(const vector<int> &sources) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setSourceWeights(const vector<int> &sourceWeights) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setOutDegree(int degree) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setDestinations(const vector<int> &destinations) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setDestWeights(const vector<int> &destWeights) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
};

class ampiCartTopology final : public ampiTopology {
 private:
  int ndims;
  vector<int> dims, periods, nbors;

 public:
  ampiCartTopology() noexcept : ndims(-1) {}

  void pup(PUP::er &p) noexcept {
    p|ndims;
    p|dims;
    p|periods;
    p|nbors;
  }

  inline int getType() const noexcept {return MPI_CART;}
  inline void dup(ampiTopology* topo) noexcept {
    CkAssert(topo->getType() == MPI_CART);
    setndims(topo->getndims());
    setdims(topo->getdims());
    setperiods(topo->getperiods());
    setnbors(topo->getnbors());
  }

  inline const vector<int> &getdims() const noexcept {return dims;}
  inline const vector<int> &getperiods() const noexcept {return periods;}
  inline int getndims() const noexcept {return ndims;}
  inline const vector<int> &getnbors() const noexcept {return nbors;}

  inline void setdims(const vector<int> &d) noexcept {dims = d; dims.shrink_to_fit();}
  inline void setperiods(const vector<int> &p) noexcept {periods = p; periods.shrink_to_fit();}
  inline void setndims(int nd) noexcept {ndims = nd;}
  inline void setnbors(const vector<int> &n) noexcept {nbors = n; nbors.shrink_to_fit();}
};

class ampiGraphTopology final : public ampiTopology {
 private:
  int nvertices;
  vector<int> index, edges, nbors;

 public:
  ampiGraphTopology() noexcept : nvertices(-1) {}

  void pup(PUP::er &p) noexcept {
    p|nvertices;
    p|index;
    p|edges;
    p|nbors;
  }

  inline int getType() const noexcept {return MPI_GRAPH;}
  inline void dup(ampiTopology* topo) noexcept {
    CkAssert(topo->getType() == MPI_GRAPH);
    setnvertices(topo->getnvertices());
    setindex(topo->getindex());
    setedges(topo->getedges());
    setnbors(topo->getnbors());
  }

  inline int getnvertices() const noexcept {return nvertices;}
  inline const vector<int> &getindex() const noexcept {return index;}
  inline const vector<int> &getedges() const noexcept {return edges;}
  inline const vector<int> &getnbors() const noexcept {return nbors;}

  inline void setnvertices(int nv) noexcept {nvertices = nv;}
  inline void setindex(const vector<int> &i) noexcept {index = i; index.shrink_to_fit();}
  inline void setedges(const vector<int> &e) noexcept {edges = e; edges.shrink_to_fit();}
  inline void setnbors(const vector<int> &n) noexcept {nbors = n; nbors.shrink_to_fit();}
};

class ampiDistGraphTopology final : public ampiTopology {
 private:
  int inDegree, outDegree;
  bool sourcesWeighted, destsWeighted;
  vector<int> sources, sourceWeights, destinations, destWeights, nbors;

 public:
  ampiDistGraphTopology() noexcept : inDegree(-1), outDegree(-1), sourcesWeighted(false), destsWeighted(false) {}

  void pup(PUP::er &p) noexcept {
    p|inDegree;
    p|outDegree;
    p|sourcesWeighted;
    p|destsWeighted;
    p|sources;
    p|sourceWeights;
    p|destinations;
    p|destWeights;
    p|nbors;
  }

  inline int getType() const noexcept {return MPI_DIST_GRAPH;}
  inline void dup(ampiTopology* topo) noexcept {
    CkAssert(topo->getType() == MPI_DIST_GRAPH);
    setAreSourcesWeighted(topo->areSourcesWeighted());
    setAreDestsWeighted(topo->areDestsWeighted());
    setInDegree(topo->getInDegree());
    setSources(topo->getSources());
    setSourceWeights(topo->getSourceWeights());
    setOutDegree(topo->getOutDegree());
    setDestinations(topo->getDestinations());
    setDestWeights(topo->getDestWeights());
    setnbors(topo->getnbors());
  }

  inline int getInDegree() const noexcept {return inDegree;}
  inline const vector<int> &getSources() const noexcept {return sources;}
  inline const vector<int> &getSourceWeights() const noexcept {return sourceWeights;}
  inline int getOutDegree() const noexcept {return outDegree;}
  inline const vector<int> &getDestinations() const noexcept {return destinations;}
  inline const vector<int> &getDestWeights() const noexcept {return destWeights;}
  inline bool areSourcesWeighted() const noexcept {return sourcesWeighted;}
  inline bool areDestsWeighted() const noexcept {return destsWeighted;}
  inline const vector<int> &getnbors() const noexcept {return nbors;}

  inline void setAreSourcesWeighted(bool v) noexcept {sourcesWeighted = v ? 1 : 0;}
  inline void setAreDestsWeighted(bool v) noexcept {destsWeighted = v ? 1 : 0;}
  inline void setInDegree(int d) noexcept {inDegree = d;}
  inline void setSources(const vector<int> &s) noexcept {sources = s; sources.shrink_to_fit();}
  inline void setSourceWeights(const vector<int> &sw) noexcept {sourceWeights = sw; sourceWeights.shrink_to_fit();}
  inline void setOutDegree(int d) noexcept {outDegree = d;}
  inline void setDestinations(const vector<int> &d) noexcept {destinations = d; destinations.shrink_to_fit();}
  inline void setDestWeights(const vector<int> &dw) noexcept {destWeights = dw; destWeights.shrink_to_fit();}
  inline void setnbors(const vector<int> &nbors_) noexcept {nbors = nbors_; nbors.shrink_to_fit();}
};

/* KeyValue class for attribute caching */
class KeyvalNode {
 public:
  void *val;
  MPI_Copy_function *copy_fn;
  MPI_Delete_function *delete_fn;
  void *extra_state;
  int refCount;
  bool isValSet;

  KeyvalNode() : val(NULL), copy_fn(NULL), delete_fn(NULL), extra_state(NULL), refCount(1), isValSet(false) { }
  KeyvalNode(MPI_Copy_function *cf, MPI_Delete_function *df, void* es) :
             val(NULL), copy_fn(cf), delete_fn(df), extra_state(es), refCount(1), isValSet(false) { }
  bool hasVal() const { return isValSet; }
  void clearVal() { isValSet = false; }
  void setVal(void *v) { val = v; isValSet = true; }
  void* getVal() const { return val; }
  void incRefCount() { refCount++; }
  int decRefCount() { CkAssert(refCount > 0); refCount--; return refCount; }
  void pup(PUP::er& p) {
    p((char *)val, sizeof(void *));
    p((char *)copy_fn, sizeof(void *));
    p((char *)delete_fn, sizeof(void *));
    p((char *)extra_state, sizeof(void *));
    p|refCount;
    p|isValSet;
  }
};

// Only store Group ranks explicitly when they can't be
// lazily and transiently created via std::iota()
class groupStruct {
 private:
  int sz; // -1 if ranks is valid, otherwise the size to pass to std::iota()
  vector<int> ranks;

 private:
  bool ranksIsIota() const noexcept {
    for (int i=0; i<ranks.size(); i++)
      if (ranks[i] != i)
        return false;
    return true;
  }

 public:
  groupStruct() noexcept : sz(0) {}
  groupStruct(int s) noexcept : sz(s) {}
  groupStruct(vector<int> r) noexcept : sz(-1), ranks(std::move(r)) {
    if (ranksIsIota()) {
      sz = ranks.size();
      ranks.clear();
    }
    ranks.shrink_to_fit();
  }
  groupStruct &operator=(const groupStruct &obj) noexcept {
    sz = obj.sz;
    ranks = obj.ranks;
    return *this;
  }
  ~groupStruct() = default;
  void pup(PUP::er& p) noexcept {
    p|sz;
    p|ranks;
  }
  bool isIota() const noexcept {return (sz != -1);}
  int operator[](int i) const noexcept {return (isIota()) ? i : ranks[i];}
  int size() const noexcept {return (isIota()) ? sz : ranks.size();}
  vector<int> getRanks() const noexcept {
    if (isIota()) {
      // Lazily create ranks:
      vector<int> tmpRanks(sz);
      std::iota(tmpRanks.begin(), tmpRanks.end(), 0);
      tmpRanks.shrink_to_fit();
      return tmpRanks;
    }
    else {
      return ranks;
    }
  }
};

enum AmpiCommType : uint8_t {
   WORLD = 0
  ,INTRA = 1
  ,INTER = 2
};

//Describes an AMPI communicator
class ampiCommStruct {
 private:
  MPI_Comm comm; //Communicator
  CkArrayID ampiID; //ID of corresponding ampi array
  int size; //Number of processes in communicator
  AmpiCommType commType; //COMM_WORLD, intracomm, intercomm?
  groupStruct indices;  //indices[r] gives the array index for rank r
  groupStruct remoteIndices;  // remote group for inter-communicator

  ampiTopology *ampiTopo; // Virtual topology
  int topoType; // Type of virtual topology: MPI_CART, MPI_GRAPH, MPI_DIST_GRAPH, or MPI_UNDEFINED

  // For communicator attributes (MPI_*_get_attr): indexed by keyval
  vector<int> keyvals;

  // For communicator names
  std::string commName;

 public:
  ampiCommStruct(int ignored=0) noexcept
    : size(-1), commType(INTRA), ampiTopo(NULL), topoType(MPI_UNDEFINED)
  {}
  ampiCommStruct(MPI_Comm comm_,const CkArrayID &id_,int size_) noexcept
    : comm(comm_), ampiID(id_),size(size_), commType(WORLD), indices(size_),
      ampiTopo(NULL), topoType(MPI_UNDEFINED)
  {}
  ampiCommStruct(MPI_Comm comm_,const CkArrayID &id_, const vector<int> &indices_) noexcept
    : comm(comm_), ampiID(id_), size(indices_.size()), commType(INTRA), indices(indices_),
      ampiTopo(NULL), topoType(MPI_UNDEFINED)
  {}
  ampiCommStruct(MPI_Comm comm_, const CkArrayID &id_, const vector<int> &indices_,
                 const vector<int> &remoteIndices_) noexcept
    : comm(comm_), ampiID(id_), size(indices_.size()), commType(INTER), indices(indices_),
      remoteIndices(remoteIndices_), ampiTopo(NULL), topoType(MPI_UNDEFINED)
  {}

  ~ampiCommStruct() noexcept {
    if (ampiTopo != NULL)
      delete ampiTopo;
  }

  // Overloaded copy constructor. Used when creating virtual topologies.
  ampiCommStruct(const ampiCommStruct &obj, int topoNumber=MPI_UNDEFINED) noexcept {
    switch (topoNumber) {
      case MPI_CART:
        ampiTopo = new ampiCartTopology();
        break;
      case MPI_GRAPH:
        ampiTopo = new ampiGraphTopology();
        break;
      case MPI_DIST_GRAPH:
        ampiTopo = new ampiDistGraphTopology();
        break;
      default:
        ampiTopo = NULL;
        break;
    }
    topoType       = topoNumber;
    comm           = obj.comm;
    ampiID         = obj.ampiID;
    size           = obj.size;
    commType       = obj.commType;
    indices        = obj.indices;
    remoteIndices  = obj.remoteIndices;
    keyvals        = obj.keyvals;
    commName       = obj.commName;
  }

  ampiCommStruct &operator=(const ampiCommStruct &obj) noexcept {
    if (this == &obj) {
      return *this;
    }
    switch (obj.topoType) {
      case MPI_CART:
        ampiTopo = new ampiCartTopology(*(static_cast<ampiCartTopology*>(obj.ampiTopo)));
        break;
      case MPI_GRAPH:
        ampiTopo = new ampiGraphTopology(*(static_cast<ampiGraphTopology*>(obj.ampiTopo)));
        break;
      case MPI_DIST_GRAPH:
        ampiTopo = new ampiDistGraphTopology(*(static_cast<ampiDistGraphTopology*>(obj.ampiTopo)));
        break;
      default:
        ampiTopo = NULL;
        break;
    }
    topoType       = obj.topoType;
    comm           = obj.comm;
    ampiID         = obj.ampiID;
    size           = obj.size;
    commType       = obj.commType;
    indices        = obj.indices;
    remoteIndices  = obj.remoteIndices;
    keyvals        = obj.keyvals;
    commName       = obj.commName;
    return *this;
  }

  const ampiTopology* getTopologyforNeighbors() const noexcept {
    return ampiTopo;
  }

  ampiTopology* getTopology() noexcept {
    return ampiTopo;
  }

  inline bool isinter() const noexcept {return commType==INTER;}
  void setArrayID(const CkArrayID &nID) noexcept {ampiID=nID;}

  MPI_Comm getComm() const noexcept {return comm;}
  inline vector<int> getIndices() const noexcept {return indices.getRanks();}
  inline vector<int> getRemoteIndices() const noexcept {return remoteIndices.getRanks();}
  vector<int> &getKeyvals() noexcept {return keyvals;}

  void setName(const char *src) noexcept {
    CkDDT_SetName(commName, src);
  }

  void getName(char *name, int *len) const noexcept {
    int length = *len = commName.size();
    memcpy(name, commName.data(), length);
    name[length] = '\0';
  }

  //Get the proxy for the entire array
  CProxy_ampi getProxy() const noexcept;

  //Get the array index for rank r in this communicator
  int getIndexForRank(int r) const noexcept {
#if CMK_ERROR_CHECKING
    if (r>=size) CkAbort("AMPI> You passed in an out-of-bounds process rank!");
#endif
    return indices[r];
  }
  int getIndexForRemoteRank(int r) const noexcept {
#if CMK_ERROR_CHECKING
    if (r>=remoteIndices.size()) CkAbort("AMPI> You passed in an out-of-bounds intercomm remote process rank!");
#endif
    return remoteIndices[r];
  }
  //Get the rank for this array index (Warning: linear time)
  int getRankForIndex(int i) const noexcept {
    if (indices.isIota()) return i;
    else {
      const vector<int>& ind = indices.getRanks();
      for (int r=0;r<ind.size();r++)
        if (ind[r]==i) return r;
      return -1; /*That index isn't in this communicator*/
    }
  }

  int getSize() const noexcept {return size;}

  void pup(PUP::er &p) noexcept {
    p|comm;
    p|ampiID;
    p|size;
    p|commType;
    p|indices;
    p|remoteIndices;
    p|keyvals;
    p|commName;
    p|topoType;
    if (topoType != MPI_UNDEFINED) {
      if (p.isUnpacking()) {
        switch (topoType) {
          case MPI_CART:
            ampiTopo = new ampiCartTopology();
            break;
          case MPI_GRAPH:
            ampiTopo = new ampiGraphTopology();
            break;
          case MPI_DIST_GRAPH:
            ampiTopo = new ampiDistGraphTopology();
            break;
          default:
            CkAbort("AMPI> Communicator has an invalid topology!");
            break;
        }
      }
      ampiTopo->pup(p);
    } else {
      ampiTopo = NULL;
    }
    if (p.isDeleting()) {
      delete ampiTopo; ampiTopo = NULL;
    }
  }
};
PUPmarshall(ampiCommStruct)

class mpi_comm_worlds{
  ampiCommStruct comms[MPI_MAX_COMM_WORLDS];
 public:
  ampiCommStruct &operator[](int i) noexcept {return comms[i];}
  void pup(PUP::er &p) noexcept {
    for (int i=0;i<MPI_MAX_COMM_WORLDS;i++)
      comms[i].pup(p);
  }
};

// group operations
inline void outputOp(const vector<int>& vec) noexcept {
  if (vec.size() > 50) {
    CkPrintf("vector too large to output!\n");
    return;
  }
  CkPrintf("output vector: size=%d {",vec.size());
  for (int i=0; i<vec.size(); i++) {
    CkPrintf(" %d ", vec[i]);
  }
  CkPrintf("}\n");
}

inline int getPosOp(int idx, const vector<int>& vec) noexcept {
  for (int r=0; r<vec.size(); r++) {
    if (vec[r] == idx) {
      return r;
    }
  }
  return MPI_UNDEFINED;
}

inline vector<int> unionOp(const vector<int>& vec1, const vector<int>& vec2) noexcept {
  vector<int> newvec(vec1);
  for (int i=0; i<vec2.size(); i++) {
    if (getPosOp(vec2[i], vec1) == MPI_UNDEFINED) {
      newvec.push_back(vec2[i]);
    }
  }
  return newvec;
}

inline vector<int> intersectOp(const vector<int>& vec1, const vector<int>& vec2) noexcept {
  vector<int> newvec;
  for (int i=0; i<vec1.size(); i++) {
    if (getPosOp(vec1[i], vec2) != MPI_UNDEFINED) {
      newvec.push_back(vec1[i]);
    }
  }
  return newvec;
}

inline vector<int> diffOp(const vector<int>& vec1, const vector<int>& vec2) noexcept {
  vector<int> newvec;
  for (int i=0; i<vec1.size(); i++) {
    if (getPosOp(vec1[i], vec2) == MPI_UNDEFINED) {
      newvec.push_back(vec1[i]);
    }
  }
  return newvec;
}

inline int* translateRanksOp(int n, const vector<int>& vec1, const int* ranks1,
                             const vector<int>& vec2, int *ret) noexcept {
  for (int i=0; i<n; i++) {
    ret[i] = (ranks1[i] == MPI_PROC_NULL) ? MPI_PROC_NULL : getPosOp(vec1[ranks1[i]], vec2);
  }
  return ret;
}

inline int compareVecOp(const vector<int>& vec1, const vector<int>& vec2) noexcept {
  int pos, ret = MPI_IDENT;
  if (vec1.size() != vec2.size()) {
    return MPI_UNEQUAL;
  }
  for (int i=0; i<vec1.size(); i++) {
    pos = getPosOp(vec1[i], vec2);
    if (pos == MPI_UNDEFINED) {
      return MPI_UNEQUAL;
    }
    else if (pos != i) {
      ret = MPI_SIMILAR;
    }
  }
  return ret;
}

inline vector<int> inclOp(int n, const int* ranks, const vector<int>& vec) noexcept {
  vector<int> retvec(n);
  for (int i=0; i<n; i++) {
    retvec[i] = vec[ranks[i]];
  }
  return retvec;
}

inline vector<int> exclOp(int n, const int* ranks, const vector<int>& vec) noexcept {
  vector<int> retvec;
  bool add = true;
  for (int j=0; j<vec.size(); j++) {
    for (int i=0; i<n; i++) {
      if (j == ranks[i]) {
        add = false;
        break;
      }
    }
    if (add) {
      retvec.push_back(vec[j]);
    }
    else {
      add = true;
    }
  }
  return retvec;
}

inline vector<int> rangeInclOp(int n, int ranges[][3], const vector<int>& vec,
                               int *flag) noexcept {
  vector<int> retvec;
  int first, last, stride;
  for (int i=0; i<n; i++) {
    first  = ranges[i][0];
    last   = ranges[i][1];
    stride = ranges[i][2];
    if (stride != 0) {
      for (int j=0; j<=(last-first)/stride; j++) {
        retvec.push_back(vec[first+stride*j]);
      }
    }
    else {
      *flag = MPI_ERR_ARG;
      return vector<int>();
    }
  }
  *flag = MPI_SUCCESS;
  return retvec;
}

inline vector<int> rangeExclOp(int n, int ranges[][3], const vector<int>& vec,
                               int *flag) noexcept {
  vector<int> ranks;
  int first, last, stride;
  for (int i=0; i<n; i++) {
    first  = ranges[i][0];
    last   = ranges[i][1];
    stride = ranges[i][2];
    if (stride != 0) {
      for (int j=0; j<=(last-first)/stride; j++) {
        ranks.push_back(first+stride*j);
      }
    }
    else {
      *flag = MPI_ERR_ARG;
      return vector<int>();
    }
  }
  *flag = MPI_SUCCESS;
  return exclOp(ranks.size(), &ranks[0], vec);
}

#include "tcharm.h"
#include "tcharmc.h"

#include "ampi.decl.h"
#include "charm-api.h"
#include <sys/stat.h> // for mkdir

extern int _mpi_nworlds;

//MPI_ANY_TAG is defined in ampi.h to MPI_TAG_UB_VALUE+1
#define MPI_ATA_SEQ_TAG     MPI_TAG_UB_VALUE+2
#define MPI_BCAST_TAG       MPI_TAG_UB_VALUE+3
#define MPI_REDN_TAG        MPI_TAG_UB_VALUE+4
#define MPI_SCATTER_TAG     MPI_TAG_UB_VALUE+5
#define MPI_SCAN_TAG        MPI_TAG_UB_VALUE+6
#define MPI_EXSCAN_TAG      MPI_TAG_UB_VALUE+7
#define MPI_ATA_TAG         MPI_TAG_UB_VALUE+8
#define MPI_NBOR_TAG        MPI_TAG_UB_VALUE+9
#define MPI_RMA_TAG         MPI_TAG_UB_VALUE+10
#define MPI_EPOCH_START_TAG MPI_TAG_UB_VALUE+11
#define MPI_EPOCH_END_TAG   MPI_TAG_UB_VALUE+12

#define AMPI_COLL_SOURCE 0
#define AMPI_COLL_COMM   MPI_COMM_WORLD

enum AmpiReqType : uint8_t {
  AMPI_INVALID_REQ = 0,
  AMPI_I_REQ       = 1,
  AMPI_ATA_REQ     = 2,
  AMPI_SEND_REQ    = 3,
  AMPI_SSEND_REQ   = 4,
  AMPI_REDN_REQ    = 5,
  AMPI_GATHER_REQ  = 6,
  AMPI_GATHERV_REQ = 7,
  AMPI_G_REQ       = 8,
#if CMK_CUDA
  AMPI_GPU_REQ     = 9
#endif
};

inline void operator|(PUP::er &p, AmpiReqType &r) {
  pup_bytes(&p, (void *)&r, sizeof(AmpiReqType));
}

enum AmpiReqSts : char {
  AMPI_REQ_PENDING   = 0,
  AMPI_REQ_BLOCKED   = 1,
  AMPI_REQ_COMPLETED = 2
};

enum AmpiSendType : char {
  BLOCKING_SEND = 0,
  I_SEND = 1,
  BLOCKING_SSEND = 2,
  I_SSEND = 3
};

#define MyAlign8(x) (((x)+7)&(~7))

/**
Represents an MPI request that has been initiated
using Isend, Irecv, Ialltoall, Send_init, etc.
*/
class AmpiRequest {
 public:
  void *buf          = nullptr;
  int count          = 0;
  MPI_Datatype type  = MPI_DATATYPE_NULL;
  int tag            = MPI_ANY_TAG; // the order must match MPI_Status
  int src            = MPI_ANY_SOURCE;
  MPI_Comm comm      = MPI_COMM_NULL;
  MPI_Request reqIdx = MPI_REQUEST_NULL;
  bool complete      = false;
  bool blocked       = false; // this req is currently blocked on

#if CMK_BIGSIM_CHARM
 public:
  void *event        = nullptr; // the event point that corresponds to this message
  int eventPe        = -1; // the PE that the event is located on
#endif

 public:
  AmpiRequest() =default;
  virtual ~AmpiRequest() =default;

  /// Activate this persistent request.
  ///  Only meaningful for persistent Ireq, SendReq, and SsendReq requests.
  virtual void start(MPI_Request reqIdx) noexcept {}

  /// Used by AmmEntry's constructor
  virtual int getTag() const noexcept { return tag; }
  virtual int getSrcRank() const noexcept { return src; }

  /// Return true if this request is finished (progress):
  virtual bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept =0;

  /// Block until this request is finished,
  ///  returning a valid MPI error code.
  virtual int wait(MPI_Status *sts) noexcept =0;

  /// Mark this request for cancellation.
  /// Supported only for IReq requests
  virtual void cancel() noexcept {}

  /// Mark this request persistent.
  /// Supported only for IReq, SendReq, and SsendReq requests
  virtual void setPersistent(bool p) noexcept {}
  virtual bool isPersistent() const noexcept { return false; }

  /// Receive an AmpiMsg
  /// Returns true if the msg payload is recv'ed, otherwise return false
  /// (if the msg is a sync msg, it can't be recv'ed until the caller
  /// acks the sender to get the real payload)
  virtual bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept =0;

  /// Receive a CkReductionMsg
  virtual void receive(ampi *ptr, CkReductionMsg *msg) noexcept =0;

  /// Receive an Rdma message
  virtual void receiveRdma(ampi *ptr, char *sbuf, int slength, int srcRank) noexcept { }

  /// Set the request's index into AmpiRequestList
  void setReqIdx(MPI_Request idx) noexcept { reqIdx = idx; }
  MPI_Request getReqIdx() const noexcept { return reqIdx; }

  /// Free the request's datatype
  void free(CkDDT* ddt) noexcept {
    if (type != MPI_DATATYPE_NULL) ddt->freeType(type);
  }

  /// Set whether the request is currently blocked on
  void setBlocked(bool b) noexcept { blocked = b; }
  bool isBlocked() const noexcept { return blocked; }

  /// Returns the type of request:
  ///  AMPI_I_REQ, AMPI_ATA_REQ, AMPI_SEND_REQ, AMPI_SSEND_REQ,
  ///  AMPI_REDN_REQ, AMPI_GATHER_REQ, AMPI_GATHERV_REQ, AMPI_G_REQ
  virtual AmpiReqType getType() const noexcept =0;

  /// Returns whether this request will need to be matched.
  /// It is used to determine whether this request should be inserted into postedReqs.
  /// AMPI_SEND_REQ, AMPI_SSEND_REQ, and AMPI_ATA_REQ should not be posted.
  virtual bool isUnmatched() const noexcept =0;

  /// Returns whether this type is pooled or not:
  /// Only AMPI_I_REQ, AMPI_SEND_REQ, and AMPI_SSEND_REQs are pooled.
  virtual bool isPooledType() const noexcept { return false; }

  /// Return the actual number of bytes that were received.
  virtual int getNumReceivedBytes(CkDDT *ddt) const noexcept {
    // by default, return number of bytes requested
    return count * ddt->getSize(type);
  }

  virtual void pup(PUP::er &p) noexcept {
    p((char *)&buf, sizeof(void *)); //supposed to work only with Isomalloc
    p(count);
    p(type);
    p(tag);
    p(src);
    p(comm);
    p(reqIdx);
    p(complete);
    p(blocked);
#if CMK_BIGSIM_CHARM
    //needed for bigsim out-of-core emulation
    //as the "log" is not moved from memory, this pointer is safe
    //to be reused
    p((char *)&event, sizeof(void *));
    p(eventPe);
#endif
  }

  virtual void print() const noexcept =0;
};

// This is used in the constructors of the AmpiRequest types below,
// assuming arguments: (MPI_Datatype type_, CkDDT* ddt_, AmpiReqSts sts_)
#define AMPI_REQUEST_COMMON_INIT           \
{                                          \
  complete = (sts_ == AMPI_REQ_COMPLETED); \
  blocked  = (sts_ == AMPI_REQ_BLOCKED);   \
  if (type_ != MPI_DATATYPE_NULL) {        \
    ddt_->getType(type_)->incRefCount();   \
  }                                        \
}

class IReq final : public AmpiRequest {
 public:
  bool cancelled  = false; // track if request is cancelled
  bool persistent = false; // Is this a persistent recv request?
  int length      = 0; // recv'ed length in bytes

  IReq(void *buf_, int count_, MPI_Datatype type_, int src_, int tag_,
       MPI_Comm comm_, CkDDT *ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf   = buf_;
    count = count_;
    type  = type_;
    src   = src_;
    tag   = tag_;
    comm  = comm_;
    AMPI_REQUEST_COMMON_INIT
  }
  IReq() =default;
  ~IReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  int wait(MPI_Status *sts) noexcept override;
  void cancel() noexcept override { if (!complete) cancelled = true; }
  AmpiReqType getType() const noexcept override { return AMPI_I_REQ; }
  bool isUnmatched() const noexcept override { return !complete; }
  bool isPooledType() const noexcept override { return true; }
  void setPersistent(bool p) noexcept override { persistent = p; }
  bool isPersistent() const noexcept override { return persistent; }
  void start(MPI_Request reqIdx) noexcept override;
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override;
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override {}
  void receiveRdma(ampi *ptr, char *sbuf, int slength, int srcRank) noexcept override;
  int getNumReceivedBytes(CkDDT *ptr) const noexcept override {
    return length;
  }
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p|cancelled;
    p|persistent;
    p|length;
  }
  void print() const noexcept override;
};

class RednReq final : public AmpiRequest {
 public:
  MPI_Op op = MPI_OP_NULL;

  RednReq(void *buf_, int count_, MPI_Datatype type_, MPI_Comm comm_,
          MPI_Op op_, CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf   = buf_;
    count = count_;
    type  = type_;
    src   = AMPI_COLL_SOURCE;
    tag   = MPI_REDN_TAG;
    comm  = comm_;
    op    = op_;
    AMPI_REQUEST_COMMON_INIT
  }
  RednReq() =default;
  ~RednReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  int wait(MPI_Status *sts) noexcept override;
  void cancel() noexcept override {}
  AmpiReqType getType() const noexcept override { return AMPI_REDN_REQ; }
  bool isUnmatched() const noexcept override { return !complete; }
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override;
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p|op;
  }
  void print() const noexcept override;
};

class GatherReq final : public AmpiRequest {
 public:
  GatherReq(void *buf_, int count_, MPI_Datatype type_, MPI_Comm comm_,
            CkDDT *ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf   = buf_;
    count = count_;
    type  = type_;
    src   = AMPI_COLL_SOURCE;
    tag   = MPI_REDN_TAG;
    comm  = comm_;
    AMPI_REQUEST_COMMON_INIT
  }
  GatherReq() =default;
  ~GatherReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  int wait(MPI_Status *sts) noexcept override;
  void cancel() noexcept override {}
  AmpiReqType getType() const noexcept override { return AMPI_GATHER_REQ; }
  bool isUnmatched() const noexcept override { return !complete; }
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override;
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
  }
  void print() const noexcept override;
};

class GathervReq final : public AmpiRequest {
 public:
  vector<int> recvCounts;
  vector<int> displs;

  GathervReq(void *buf_, int count_, MPI_Datatype type_, MPI_Comm comm_, const int *rc,
             const int *d, CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf   = buf_;
    count = count_;
    type  = type_;
    src   = AMPI_COLL_SOURCE;
    tag   = MPI_REDN_TAG;
    comm  = comm_;
    recvCounts.assign(rc, rc+count);
    displs.assign(d, d+count);
    AMPI_REQUEST_COMMON_INIT
  }
  GathervReq() =default;
  ~GathervReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  int wait(MPI_Status *sts) noexcept override;
  AmpiReqType getType() const noexcept override { return AMPI_GATHERV_REQ; }
  bool isUnmatched() const noexcept override { return !complete; }
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override;
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p|recvCounts;
    p|displs;
  }
  void print() const noexcept override;
};

class SendReq final : public AmpiRequest {
  bool persistent = false; // is this a persistent send request?

 public:
  SendReq(MPI_Datatype type_, MPI_Comm comm_, CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    type = type_;
    comm = comm_;
    AMPI_REQUEST_COMMON_INIT
  }
  SendReq(void* buf_, int count_, MPI_Datatype type_, int dest_, int tag_,
          MPI_Comm comm_, CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf   = buf_;
    count = count_;
    type  = type_;
    src   = dest_;
    tag   = tag_;
    comm  = comm_;
    AMPI_REQUEST_COMMON_INIT
  }
  SendReq() noexcept {}
  ~SendReq() noexcept {}
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  int wait(MPI_Status *sts) noexcept override;
  void setPersistent(bool p) noexcept override { persistent = p; }
  bool isPersistent() const noexcept override { return persistent; }
  void start(MPI_Request reqIdx) noexcept override;
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override {}
  AmpiReqType getType() const noexcept override { return AMPI_SEND_REQ; }
  bool isUnmatched() const noexcept override { return false; }
  bool isPooledType() const noexcept override { return true; }
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p|persistent;
  }
  void print() const noexcept override;
};

class SsendReq final : public AmpiRequest {
 private:
  bool persistent = false; // is this a persistent Ssend request?
 public:
  int destRank = MPI_PROC_NULL;

 public:
  SsendReq(MPI_Datatype type_, MPI_Comm comm_, CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    type = type_;
    comm = comm_;
    AMPI_REQUEST_COMMON_INIT
  }
  SsendReq(const void* buf_, int count_, MPI_Datatype type_, int dest_, int tag_, MPI_Comm comm_,
           CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf      = (void*)buf_;
    count    = count_;
    type     = type_;
    tag      = tag_;
    comm     = comm_;
    destRank = dest_;
    AMPI_REQUEST_COMMON_INIT
  }
  SsendReq(const void* buf_, int count_, MPI_Datatype type_, int dest_, int tag_, MPI_Comm comm_,
           int src_, CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf      = (void*)buf_;
    count    = count_;
    type     = type_;
    src      = src_;
    tag      = tag_;
    comm     = comm_;
    destRank = dest_;
    AMPI_REQUEST_COMMON_INIT
  }
  SsendReq() =default;
  ~SsendReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  int wait(MPI_Status *sts) noexcept override;
  void setPersistent(bool p) noexcept override { persistent = p; }
  bool isPersistent() const noexcept override { return persistent; }
  void start(MPI_Request reqIdx) noexcept override;
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override {}
  AmpiReqType getType() const noexcept override { return AMPI_SSEND_REQ; }
  bool isUnmatched() const noexcept override { return false; }
  bool isPooledType() const noexcept override { return true; }
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p|persistent;
    p|destRank;
  }
  void print() const noexcept override;
};

#if CMK_CUDA
class GPUReq : public AmpiRequest {
 public:
  GPUReq() noexcept;
  ~GPUReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  int wait(MPI_Status *sts) noexcept override;
  void receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override;
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override;
  AmpiReqType getType() const noexcept override { return AMPI_GPU_REQ; }
  bool isUnmatched() const noexcept override { return false; }
  void setComplete() noexcept;
  void print() const noexcept override;
};
#endif

class ATAReq final : public AmpiRequest {
 public:
  vector<MPI_Request> reqs;

  ATAReq(int numReqs_) noexcept : reqs(numReqs_) {}
  ATAReq() =default;
  ~ATAReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  int wait(MPI_Status *sts) noexcept override;
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override {}
  int getCount() const noexcept { return reqs.size(); }
  AmpiReqType getType() const noexcept override { return AMPI_ATA_REQ; }
  bool isUnmatched() const noexcept override { return false; }
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p|reqs;
  }
  void print() const noexcept override;
};

class GReq final : public AmpiRequest {
 private:
  MPI_Grequest_query_function* queryFn;
  MPI_Grequest_free_function* freeFn;
  MPI_Grequest_cancel_function* cancelFn;
  MPIX_Grequest_poll_function* pollFn;
  MPIX_Grequest_wait_function* waitFn;
  void* extraState;

 public:
  GReq(MPI_Grequest_query_function* q, MPI_Grequest_free_function* f, MPI_Grequest_cancel_function* c, void* es) noexcept
    : queryFn(q), freeFn(f), cancelFn(c), pollFn(nullptr), waitFn(nullptr), extraState(es) {}
  GReq(MPI_Grequest_query_function *q, MPI_Grequest_free_function* f, MPI_Grequest_cancel_function* c, MPIX_Grequest_poll_function* p, void* es) noexcept
    : queryFn(q), freeFn(f), cancelFn(c), pollFn(p), waitFn(nullptr), extraState(es) {}
  GReq(MPI_Grequest_query_function *q, MPI_Grequest_free_function* f, MPI_Grequest_cancel_function* c, MPIX_Grequest_poll_function* p, MPIX_Grequest_wait_function* w, void* es) noexcept
    : queryFn(q), freeFn(f), cancelFn(c), pollFn(p), waitFn(w), extraState(es) {}
  GReq() =default;
  ~GReq() noexcept { (*freeFn)(extraState); }
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  int wait(MPI_Status *sts) noexcept override;
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override {}
  void cancel() noexcept override { (*cancelFn)(extraState, complete); }
  AmpiReqType getType() const noexcept override { return AMPI_G_REQ; }
  bool isUnmatched() const noexcept override { return false; }
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p((char *)queryFn, sizeof(void *));
    p((char *)freeFn, sizeof(void *));
    p((char *)cancelFn, sizeof(void *));
    p((char *)pollFn, sizeof(void *));
    p((char *)waitFn, sizeof(void *));
    p((char *)extraState, sizeof(void *));
  }
  void print() const noexcept override;
};

class AmpiRequestPool;

class AmpiRequestList {
 private:
  vector<AmpiRequest*> reqs; // indexed by MPI_Request
  int startIdx; // start next search from this index
  AmpiRequestPool* reqPool;
 public:
  AmpiRequestList() noexcept : startIdx(0) {}
  AmpiRequestList(int size, AmpiRequestPool* reqPoolPtr) noexcept
    : reqs(size), startIdx(0), reqPool(reqPoolPtr) {}
  ~AmpiRequestList() noexcept {}

  inline AmpiRequest* operator[](int n) noexcept {
#if CMK_ERROR_CHECKING
    return reqs.at(n);
#else
    return reqs[n];
#endif
  }
  void free(AmpiRequestPool& reqPool, int idx, CkDDT *ddt) noexcept;
  void freeNonPersReq(int &idx) noexcept;
  inline int insert(AmpiRequest* req) noexcept {
    for (int i=startIdx; i<reqs.size(); i++) {
      if (reqs[i] == NULL) {
        req->setReqIdx(i);
        reqs[i] = req;
        startIdx = i+1;
        return i;
      }
    }
    reqs.push_back(req);
    int idx = reqs.size()-1;
    req->setReqIdx(idx);
    startIdx = idx+1;
    return idx;
  }

  inline void checkRequest(MPI_Request idx) const noexcept {
    if (idx != MPI_REQUEST_NULL && (idx < 0 || idx >= reqs.size()))
      CkAbort("Invalid MPI_Request\n");
  }

  inline void unblockReqs(MPI_Request *requests, int numReqs) noexcept {
    for (int i=0; i<numReqs; i++) {
      if (requests[i] != MPI_REQUEST_NULL) {
        reqs[requests[i]]->setBlocked(false);
      }
    }
  }

  void pup(PUP::er &p, AmpiRequestPool* reqPool) noexcept;

  void print() const noexcept {
    for (int i=0; i<reqs.size(); i++) {
      if (reqs[i] == NULL) continue;
      CkPrintf("AmpiRequestList Element %d [%p]: \n", i+1, reqs[i]);
      reqs[i]->print();
    }
  }
};

//A simple memory buffer
class memBuf {
  CkVec<char> buf;
 public:
  memBuf() =default;
  memBuf(int size) noexcept : buf(size) {}
  void setSize(int s) noexcept {buf.resize(s);}
  int getSize() const noexcept {return buf.size();}
  const void *getData() const noexcept {return (const void *)&buf[0];}
  void *getData() noexcept {return (void *)&buf[0];}
};

template <class T>
inline void pupIntoBuf(memBuf &b,T &t) noexcept {
  PUP::sizer ps;ps|t;
  b.setSize(ps.size());
  PUP::toMem pm(b.getData()); pm|t;
}

template <class T>
inline void pupFromBuf(const void *data,T &t) noexcept {
  PUP::fromMem p(data); p|t;
}

#define COLL_SEQ_IDX      -1

class AmpiMsgPool;

class AmpiMsg final : public CMessage_AmpiMsg {
 private:
  int ssendReq; //Index to the sender's request (MPI_REQUEST_NULL if no request)
  int tag; //MPI tag
  int srcRank; //Communicator rank for source
  int length; //Number of bytes in this message
  int origLength; // true size of allocation
  MPI_Comm comm; // Communicator
 public:
  char *data; //Payload
#if CMK_BIGSIM_CHARM
 public:
  void *event;
  int  eventPe; // the PE that the event is located
#endif

 public:
  AmpiMsg() noexcept { data = NULL; }
  AmpiMsg(int sreq, int t, int sRank, int l) noexcept :
    ssendReq(sreq), tag(t), srcRank(sRank), length(l), origLength(l)
  { /* only called from AmpiMsg::pup() since the refnum (seq) will get pup'ed by the runtime */ }
  AmpiMsg(CMK_REFNUM_TYPE seq, int sreq, int t, int sRank, int l) noexcept :
    ssendReq(sreq), tag(t), srcRank(sRank), length(l), origLength(l)
  { CkSetRefNum(this, seq); }
  inline bool isSsend() const noexcept { return (ssendReq >= 0); }
  inline void setSsendReq(int s) noexcept { ssendReq = s; }
  inline void setSeq(CMK_REFNUM_TYPE s) noexcept { CkAssert(s >= 0); UsrToEnv(this)->setRef(s); }
  inline void setSrcRank(int sr) noexcept { srcRank = sr; }
  inline void setLength(int l) noexcept { length = l; }
  inline void setTag(int t) noexcept { tag = t; }
  inline void setComm(MPI_Comm c) noexcept { comm = c; }
  inline CMK_REFNUM_TYPE getSeq() const noexcept { return UsrToEnv(this)->getRef(); }
  inline int getSsendReq() const noexcept { return ssendReq; }
  inline int getSeqIdx() const noexcept {
    // seqIdx is srcRank, unless this message was part of a collective
    if (tag >= MPI_BCAST_TAG && tag <= MPI_ATA_TAG) {
      return COLL_SEQ_IDX;
    }
    else {
      return srcRank;
    }
  }
  inline int getSrcRank() const noexcept { return srcRank; }
  inline int getLength() const noexcept { return length; }
  inline char* getData() const noexcept { return data; }
  inline int getTag() const noexcept { return tag; }
  inline MPI_Comm getComm() const noexcept { return comm; }
  static AmpiMsg* pup(PUP::er &p, AmpiMsg *m) noexcept
  {
    int ref, ssendReq, tag, srcRank, length, origLength;
    MPI_Comm comm;
    if(p.isPacking() || p.isSizing()) {
      ref = CkGetRefNum(m);
      ssendReq = m->ssendReq;
      tag = m->tag;
      srcRank = m->srcRank;
      length = m->length;
      origLength = m->origLength;
      comm = m->comm;
    }
    p(ref); p(ssendReq); p(tag); p(srcRank); p(length); p(origLength); p(comm);
    if(p.isUnpacking()) {
      m = new (origLength, 0) AmpiMsg(ref, ssendReq, tag, srcRank, origLength);
      m->setLength(length);
      m->setComm(comm);
    }
    p(m->data, length);
    if(p.isDeleting()) {
      delete m;
      m = 0;
    }
    return m;
  }

  friend AmpiMsgPool;
};

#define AMPI_MSG_POOL_SIZE   32 // Max # of AmpiMsg's allowed in the pool
#define AMPI_POOLED_MSG_SIZE 64 // Max # of Bytes in pooled msgs' payload

class AmpiMsgPool {
 private:
  std::forward_list<AmpiMsg *> msgs; // list of free msgs
  int msgLength; // AmpiMsg::length of messages in the pool
  int maxMsgs; // max # of msgs in the pool
  int currMsgs; // current # of msgs in the pool

 public:
  AmpiMsgPool(int _numMsgs = 0, int _msgLength = 0) noexcept
    : msgLength(_msgLength), maxMsgs(_numMsgs), currMsgs(0) {}
  ~AmpiMsgPool() =default;
  inline void clear() noexcept {
    while (!msgs.empty()) {
      delete msgs.front();
      msgs.pop_front();
    }
    currMsgs = 0;
  }
  inline AmpiMsg* newAmpiMsg(CMK_REFNUM_TYPE seq, int ssendReq, int tag, int srcRank, int len) noexcept {
    if (msgs.empty() || msgs.front()->origLength < len) {
      int newlen = std::max(msgLength, len);
      AmpiMsg* msg = new (newlen, 0) AmpiMsg(seq, ssendReq, tag, srcRank, newlen);
      msg->setLength(len);
      return msg;
    } else {
      AmpiMsg* msg = msgs.front();
      msgs.pop_front();
      currMsgs--;
      msg->setSeq(seq);
      msg->setSsendReq(ssendReq);
      msg->setTag(tag);
      msg->setSrcRank(srcRank);
      msg->setLength(len);
      return msg;
    }
  }
  inline void deleteAmpiMsg(AmpiMsg* msg) noexcept {
    /* msg->origLength is the true size of the message's data buffer, while
     * msg->length is the space taken by the payload within it. */
    if (currMsgs != maxMsgs && msg->origLength >= msgLength && msg->origLength < 2*msgLength) {
      msgs.push_front(msg);
      currMsgs++;
    } else {
      delete msg;
    }
  }
  void pup(PUP::er& p) {
    p|msgLength;
    p|maxMsgs;
    // Don't PUP the msgs in the free list or currMsgs, let the pool fill lazily
  }
};

// Number of requests in the pool
#ifndef AMPI_REQ_POOL_SIZE
#define AMPI_REQ_POOL_SIZE 64
#endif

// Helper macro for pool size and alignment calculations
#define DefinePooledReqX(name, func) \
static const size_t ireq##name = func(IReq); \
static const size_t sreq##name = func(SendReq); \
static const size_t ssreq##name = func(SsendReq); \
static const size_t pooledReq##name = (ireq##name >= sreq##name && ireq##name >= ssreq##name) ? ireq##name : \
                                      (sreq##name >= ireq##name && sreq##name >= ssreq##name) ? sreq##name : \
                                      (ssreq##name);

// This defines 'static const size_t pooledReqSize = ... ;'
DefinePooledReqX(Size, sizeof)

// This defines 'static const size_t pooledReqAlign = ... ;'
DefinePooledReqX(Align, alignof)

// Pool of IReq, SendReq, and SsendReq objects:
// These are different sizes, but we use a single pool for them so
// that iteration over these objects is fast, as in AMPI_Waitall.
// We also try to always allocate new requests from the start to the end
// of the pool, so that forward iteration over requests is fast.
class AmpiRequestPool {
 private:
  std::bitset<AMPI_REQ_POOL_SIZE> validReqs; // reqs in the pool are either valid (being used by a real req) or invalid
  int startIdx = 0; // start next search from this index
  alignas(pooledReqAlign) std::array<char, AMPI_REQ_POOL_SIZE*pooledReqSize> reqs; // pool of memory for requests

 public:
  AmpiRequestPool() =default;
  ~AmpiRequestPool() =default;
  template <typename T, typename... Args>
  inline T* newReq(Args&&... args) noexcept {
    if (validReqs.all()) {
      return new T(std::forward<Args>(args)...);
    } else {
      for (int i=startIdx; i<validReqs.size(); i++) {
        if (!validReqs[i]) {
          validReqs[i] = 1;
          startIdx = i+1;
          T* req = new (&reqs[i*pooledReqSize]) T(std::forward<Args>(args)...);
          return req;
        }
      }
      CkAbort("AMPI> failed to find a free request in pool!");
      return NULL;
    }
  }
  inline void deleteReq(AmpiRequest* req) noexcept {
    if (req->isPooledType() &&
        ((char*)req >= &reqs.front() && (char*)req <= &reqs.back()))
    {
      int idx = (int)((intptr_t)req - (intptr_t)&reqs[0]) / pooledReqSize;
      validReqs[idx] = 0;
      startIdx = std::min(idx, startIdx);
    } else {
      delete req;
    }
  }
  void pup(PUP::er& p) noexcept {
    // Nothing to do here, because AmpiRequestList::pup will be the
    // one to actually PUP the AmpiRequest objects to/from the pool
  }
};

/**
  Our local representation of another AMPI
 array element.  Used to keep track of incoming
 and outgoing message sequence numbers, and
 the out-of-order message list.
*/
class AmpiOtherElement {
private:
  /// Next incoming and outgoing message sequence number
  CMK_REFNUM_TYPE seqIncoming, seqOutgoing;

  /// Number of messages in out-of-order queue (normally 0)
  uint16_t numOutOfOrder;

public:
  /// seqIncoming starts from 1, b/c 0 means unsequenced
  /// seqOutgoing starts from 0, b/c this will be incremented for the first real seq #
  AmpiOtherElement() noexcept : seqIncoming(1), seqOutgoing(0), numOutOfOrder(0) {}

  /// Handle wrap around of unsigned type CMK_REFNUM_TYPE
  inline void incSeqIncoming() noexcept { seqIncoming++; if (seqIncoming==0) seqIncoming=1; }
  inline CMK_REFNUM_TYPE getSeqIncoming() const noexcept { return seqIncoming; }

  inline void incSeqOutgoing() noexcept { seqOutgoing++; if (seqOutgoing==0) seqOutgoing=1; }
  inline void decSeqOutgoing() noexcept { seqOutgoing--; if (seqOutgoing==0) seqOutgoing=std::numeric_limits<CMK_REFNUM_TYPE>::max(); }
  inline CMK_REFNUM_TYPE getSeqOutgoing() const noexcept { return seqOutgoing; }

  inline void incNumOutOfOrder() noexcept { numOutOfOrder++; }
  inline void decNumOutOfOrder() noexcept { numOutOfOrder--; }
  inline uint16_t getNumOutOfOrder() const noexcept { return numOutOfOrder; }
};
PUPbytes(AmpiOtherElement)

class AmpiSeqQ : private CkNoncopyable {
  CkMsgQ<AmpiMsg> out; // all out of order messages
  std::unordered_map<int, AmpiOtherElement> elements; // element info: indexed by seqIdx (comm rank)

public:
  AmpiSeqQ() =default;
  AmpiSeqQ(int commSize) noexcept {
    elements.reserve(std::min(commSize, 64));
  }
  ~AmpiSeqQ() =default;
  void pup(PUP::er &p) noexcept;

  /// Insert this message in the table.  Returns the number
  /// of messages now available for the element.
  ///   If 0, the message was out-of-order and is buffered.
  ///   If 1, this message can be immediately processed.
  ///   If >1, this message can be immediately processed,
  ///     and you should call "getOutOfOrder" repeatedly.
  inline int put(int seqIdx, AmpiMsg *msg) noexcept {
    AmpiOtherElement &el = elements[seqIdx];
    if (msg->getSeq() == el.getSeqIncoming()) { // In order:
      el.incSeqIncoming();
      return 1+el.getNumOutOfOrder();
    }
    else { // Out of order: stash message
      putOutOfOrder(seqIdx, msg);
      return 0;
    }
  }

  /// Is this message in order (return >0) or not (return 0)?
  /// Same as put() except we don't call putOutOfOrder() here,
  /// so the caller should do that separately
  inline int putIfInOrder(int srcRank, CMK_REFNUM_TYPE seq) noexcept {
    AmpiOtherElement &el = elements[srcRank];
    if (seq == el.getSeqIncoming()) { // In order:
      el.incSeqIncoming();
      return 1+el.getNumOutOfOrder();
    }
    else { // Out of order: caller should stash message
      return 0;
    }
  }

  /// Is this in-order?
  inline bool isInOrder(int seqIdx, CMK_REFNUM_TYPE seq) noexcept {
    return (seq == elements[seqIdx].getSeqIncoming());
  }

  /// Get an out-of-order message from the table.
  /// (in-order messages never go into the table)
  AmpiMsg *getOutOfOrder(int seqIdx) noexcept;

  /// Stash an out-of-order message
  void putOutOfOrder(int seqIdx, AmpiMsg *msg) noexcept;

  /// Increment the outgoing sequence number.
  inline void incCollSeqOutgoing() noexcept {
    elements[COLL_SEQ_IDX].incSeqOutgoing();
  }

  /// Return the next outgoing sequence number, and increment it.
  inline CMK_REFNUM_TYPE nextOutgoing(int destRank) noexcept {
    AmpiOtherElement &el = elements[destRank];
    el.incSeqOutgoing();
    return el.getSeqOutgoing();
  }

  /// Reset the outgoing sequence number to its previous value.
  inline void resetOutgoing(int destRank) noexcept {
    elements[destRank].decSeqOutgoing();
  }
};
PUPmarshall(AmpiSeqQ)


inline CProxy_ampi ampiCommStruct::getProxy() const noexcept {return ampiID;}
const ampiCommStruct &universeComm2CommStruct(MPI_Comm universeNo) noexcept;

// Max value of a predefined MPI_Op (values defined in ampi.h)
#define AMPI_MAX_PREDEFINED_OP 13

/*
An ampiParent holds all the communicators and the TCharm thread
for its children, which are bound to it.
*/
class ampiParent final : public CBase_ampiParent {
 private:
  TCharm *thread;
  CProxy_TCharm threads;

 public: // Communication state:
  int numBlockedReqs; // number of requests currently blocked on
  bool resumeOnRecv, resumeOnColl;
  AmpiRequestList ampiReqs;
  AmpiRequestPool reqPool;
  AmpiRequest *blockingReq;
  CkDDT myDDT;

 private:
  MPI_Comm worldNo; //My MPI_COMM_WORLD
  ampi *worldPtr; //AMPI element corresponding to MPI_COMM_WORLD

  CkPupPtrVec<ampiCommStruct> splitComm;     //Communicators from MPI_Comm_split
  CkPupPtrVec<ampiCommStruct> groupComm;     //Communicators from MPI_Comm_group
  CkPupPtrVec<ampiCommStruct> cartComm;      //Communicators from MPI_Cart_create
  CkPupPtrVec<ampiCommStruct> graphComm;     //Communicators from MPI_Graph_create
  CkPupPtrVec<ampiCommStruct> distGraphComm; //Communicators from MPI_Dist_graph_create
  CkPupPtrVec<ampiCommStruct> interComm;     //Communicators from MPI_Intercomm_create
  CkPupPtrVec<ampiCommStruct> intraComm;     //Communicators from MPI_Intercomm_merge

  CkPupPtrVec<groupStruct> groups; // "Wild" groups that don't have a communicator
  CkPupPtrVec<WinStruct> winStructList; //List of windows for one-sided communication
  CkPupPtrVec<InfoStruct> infos; // list of all MPI_Infos
  const std::array<MPI_User_function*, AMPI_MAX_PREDEFINED_OP+1>& predefinedOps; // owned by ampiNodeMgr
  vector<OpStruct> userOps; // list of any user-defined MPI_Ops
  vector<AmpiMsg *> matchedMsgs; // for use with MPI_Mprobe and MPI_Mrecv

  /* MPI_*_get_attr C binding returns a *pointer* to an integer,
   *  so there needs to be some storage somewhere to point to.
   * All builtin keyvals are ints, except for MPI_WIN_BASE, which
   *  is a pointer, and MPI_WIN_SIZE, which is an MPI_Aint. */
  int* kv_builtin_storage;
  MPI_Aint* win_size_storage;
  void** win_base_storage;
  CkPupPtrVec<KeyvalNode> kvlist;
  void* bsendBuffer;   // NOTE: we don't actually use this for buffering of MPI_Bsend's,
  int bsendBufferSize; //       we only keep track of it to return it from MPI_Buffer_detach

  // Intercommunicator creation:
  bool isTmpRProxySet;
  CProxy_ampi tmpRProxy;

  MPI_MigrateFn userAboutToMigrateFn, userJustMigratedFn;

 public:
  bool ampiInitCallDone;

 private:
  bool kv_set_builtin(int keyval, void* attribute_val) noexcept;
  bool kv_get_builtin(int keyval) noexcept;

 public:
  void prepareCtv() noexcept;
  TCharm* getThread() noexcept { return thread; }

  MPI_Message putMatchedMsg(AmpiMsg* msg) noexcept {
    // Search thru matchedMsgs for any NULL ones first:
    for (int i=0; i<matchedMsgs.size(); i++) {
      if (matchedMsgs[i] == NULL) {
        matchedMsgs[i] = msg;
        return i;
      }
    }
    // No NULL entries, so create a new one:
    matchedMsgs.push_back(msg);
    return matchedMsgs.size() - 1;
  }
  AmpiMsg* getMatchedMsg(MPI_Message message) noexcept {
    if (message == MPI_MESSAGE_NO_PROC || message == MPI_MESSAGE_NULL) {
      return NULL;
    }
    CkAssert(message >= 0 && message < matchedMsgs.size());
    AmpiMsg* msg = matchedMsgs[message];
    // Mark this matchedMsg index NULL and free from back of vector:
    matchedMsgs[message] = NULL;
    while (matchedMsgs.back() == NULL) {
      matchedMsgs.pop_back();
    }
    return msg;
  }

  inline void attachBuffer(void *buffer, int size) noexcept {
    bsendBuffer = buffer;
    bsendBufferSize = size;
  }
  inline void detachBuffer(void *buffer, int *size) noexcept {
    *(void **)buffer = bsendBuffer;
    *size = bsendBufferSize;
  }
  inline bool isSplit(MPI_Comm comm) const noexcept {
    return (comm>=MPI_COMM_FIRST_SPLIT && comm<MPI_COMM_FIRST_GROUP);
  }
  const ampiCommStruct &getSplit(MPI_Comm comm) const noexcept {
    int idx=comm-MPI_COMM_FIRST_SPLIT;
    if (idx>=splitComm.size()) CkAbort("Bad split communicator used");
    return *splitComm[idx];
  }
  void splitChildRegister(const ampiCommStruct &s) noexcept;

  inline bool isGroup(MPI_Comm comm) const noexcept {
    return (comm>=MPI_COMM_FIRST_GROUP && comm<MPI_COMM_FIRST_CART);
  }
  const ampiCommStruct &getGroup(MPI_Comm comm) const noexcept {
    int idx=comm-MPI_COMM_FIRST_GROUP;
    if (idx>=groupComm.size()) CkAbort("Bad group communicator used");
    return *groupComm[idx];
  }
  void groupChildRegister(const ampiCommStruct &s) noexcept;
  inline bool isInGroups(MPI_Group group) const noexcept {
    return (group>=0 && group<groups.size());
  }

  void cartChildRegister(const ampiCommStruct &s) noexcept;
  void graphChildRegister(const ampiCommStruct &s) noexcept;
  void distGraphChildRegister(const ampiCommStruct &s) noexcept;
  void interChildRegister(const ampiCommStruct &s) noexcept;
  void intraChildRegister(const ampiCommStruct &s) noexcept;

 public:
  ampiParent(MPI_Comm worldNo_,CProxy_TCharm threads_,int nRanks_) noexcept;
  ampiParent(CkMigrateMessage *msg) noexcept;
  void ckAboutToMigrate() noexcept;
  void ckJustMigrated() noexcept;
  void ckJustRestored() noexcept;
  void setUserAboutToMigrateFn(MPI_MigrateFn f) noexcept;
  void setUserJustMigratedFn(MPI_MigrateFn f) noexcept;
  ~ampiParent() noexcept;

  //Children call this when they are first created, or just migrated
  TCharm *registerAmpi(ampi *ptr,ampiCommStruct s,bool forMigration) noexcept;

  // exchange proxy info between two ampi proxies
  void ExchangeProxy(CProxy_ampi rproxy) noexcept {
    if(!isTmpRProxySet){ tmpRProxy=rproxy; isTmpRProxySet=true; }
    else{ tmpRProxy.setRemoteProxy(rproxy); rproxy.setRemoteProxy(tmpRProxy); isTmpRProxySet=false; }
  }

  //Grab the next available split/group communicator
  MPI_Comm getNextSplit() const noexcept {return MPI_COMM_FIRST_SPLIT+splitComm.size();}
  MPI_Comm getNextGroup() const noexcept {return MPI_COMM_FIRST_GROUP+groupComm.size();}
  MPI_Comm getNextCart() const noexcept {return MPI_COMM_FIRST_CART+cartComm.size();}
  MPI_Comm getNextGraph() const noexcept {return MPI_COMM_FIRST_GRAPH+graphComm.size();}
  MPI_Comm getNextDistGraph() const noexcept {return MPI_COMM_FIRST_DIST_GRAPH+distGraphComm.size();}
  MPI_Comm getNextInter() const noexcept {return MPI_COMM_FIRST_INTER+interComm.size();}
  MPI_Comm getNextIntra() const noexcept {return MPI_COMM_FIRST_INTRA+intraComm.size();}

  inline bool isCart(MPI_Comm comm) const noexcept {
    return (comm>=MPI_COMM_FIRST_CART && comm<MPI_COMM_FIRST_GRAPH);
  }
  ampiCommStruct &getCart(MPI_Comm comm) const noexcept {
    int idx=comm-MPI_COMM_FIRST_CART;
    if (idx>=cartComm.size()) CkAbort("AMPI> Bad cartesian communicator used!\n");
    return *cartComm[idx];
  }
  inline bool isGraph(MPI_Comm comm) const noexcept {
    return (comm>=MPI_COMM_FIRST_GRAPH && comm<MPI_COMM_FIRST_DIST_GRAPH);
  }
  ampiCommStruct &getGraph(MPI_Comm comm) const noexcept {
    int idx=comm-MPI_COMM_FIRST_GRAPH;
    if (idx>=graphComm.size()) CkAbort("AMPI> Bad graph communicator used!\n");
    return *graphComm[idx];
  }
  inline bool isDistGraph(MPI_Comm comm) const noexcept {
    return (comm >= MPI_COMM_FIRST_DIST_GRAPH && comm < MPI_COMM_FIRST_INTER);
  }
  ampiCommStruct &getDistGraph(MPI_Comm comm) const noexcept {
    int idx = comm-MPI_COMM_FIRST_DIST_GRAPH;
    if (idx>=distGraphComm.size()) CkAbort("Bad distributed graph communicator used");
    return *distGraphComm[idx];
  }
  inline bool isInter(MPI_Comm comm) const noexcept {
    return (comm>=MPI_COMM_FIRST_INTER && comm<MPI_COMM_FIRST_INTRA);
  }
  const ampiCommStruct &getInter(MPI_Comm comm) const noexcept {
    int idx=comm-MPI_COMM_FIRST_INTER;
    if (idx>=interComm.size()) CkAbort("AMPI> Bad inter-communicator used!\n");
    return *interComm[idx];
  }
  inline bool isIntra(MPI_Comm comm) const noexcept {
    return (comm>=MPI_COMM_FIRST_INTRA && comm<MPI_COMM_FIRST_RESVD);
  }
  const ampiCommStruct &getIntra(MPI_Comm comm) const noexcept {
    int idx=comm-MPI_COMM_FIRST_INTRA;
    if (idx>=intraComm.size()) CkAbort("Bad intra-communicator used");
    return *intraComm[idx];
  }

  void pup(PUP::er &p) noexcept;

  void startCheckpoint(const char* dname) noexcept;
  void Checkpoint(int len, const char* dname) noexcept;
  void ResumeThread() noexcept;
  TCharm* getTCharmThread() const noexcept {return thread;}
  CMI_WARN_UNUSED_RESULT inline ampiParent* blockOnRecv() noexcept;
  inline CkDDT* getDDT() noexcept { return &myDDT; }

#if CMK_LBDB_ON
  void setMigratable(bool mig) noexcept {
    thread->setMigratable(mig);
  }
#endif

  const ampiCommStruct &getWorldStruct() const noexcept;

  inline const ampiCommStruct &comm2CommStruct(MPI_Comm comm) const noexcept {
    if (comm==MPI_COMM_WORLD) return getWorldStruct();
    if (comm==worldNo) return getWorldStruct();
    if (isSplit(comm)) return getSplit(comm);
    if (isGroup(comm)) return getGroup(comm);
    if (isCart(comm)) return getCart(comm);
    if (isGraph(comm)) return getGraph(comm);
    if (isDistGraph(comm)) return getDistGraph(comm);
    if (isInter(comm)) return getInter(comm);
    if (isIntra(comm)) return getIntra(comm);
    return universeComm2CommStruct(comm);
  }

  inline vector<int>& getKeyvals(MPI_Comm comm) noexcept {
    ampiCommStruct &cs = *(ampiCommStruct *)&comm2CommStruct(comm);
    return cs.getKeyvals();
  }

  inline ampi *comm2ampi(MPI_Comm comm) const noexcept {
    if (comm==MPI_COMM_WORLD) return worldPtr;
    if (comm==worldNo) return worldPtr;
    if (isSplit(comm)) {
      const ampiCommStruct &st=getSplit(comm);
      return st.getProxy()[thisIndex].ckLocal();
    }
    if (isGroup(comm)) {
      const ampiCommStruct &st=getGroup(comm);
      return st.getProxy()[thisIndex].ckLocal();
    }
    if (isCart(comm)) {
      const ampiCommStruct &st = getCart(comm);
      return st.getProxy()[thisIndex].ckLocal();
    }
    if (isGraph(comm)) {
      const ampiCommStruct &st = getGraph(comm);
      return st.getProxy()[thisIndex].ckLocal();
    }
    if (isDistGraph(comm)) {
      const ampiCommStruct &st = getDistGraph(comm);
      return st.getProxy()[thisIndex].ckLocal();
    }
    if (isInter(comm)) {
      const ampiCommStruct &st=getInter(comm);
      return st.getProxy()[thisIndex].ckLocal();
    }
    if (isIntra(comm)) {
      const ampiCommStruct &st=getIntra(comm);
      return st.getProxy()[thisIndex].ckLocal();
    }
    if (comm>MPI_COMM_WORLD) return worldPtr; //Use MPI_WORLD ampi for cross-world messages:
    CkAbort("Invalid communicator used!");
    return NULL;
  }

  inline bool hasComm(const MPI_Group group) const noexcept {
    MPI_Comm comm = (MPI_Comm)group;
    return ( comm==MPI_COMM_WORLD || comm==worldNo || isSplit(comm) || isGroup(comm) ||
             isCart(comm) || isGraph(comm) || isDistGraph(comm) || isIntra(comm) );
    //isInter omitted because its comm number != its group number
  }
  inline vector<int> group2vec(MPI_Group group) const noexcept {
    if (group == MPI_GROUP_NULL || group == MPI_GROUP_EMPTY) {
      return vector<int>();
    }
    else if (hasComm(group)) {
      return comm2CommStruct((MPI_Comm)group).getIndices();
    }
    else {
      CkAssert(isInGroups(group));
      return groups[group]->getRanks();
    }
  }
  inline MPI_Group saveGroupStruct(const vector<int>& vec) noexcept {
    if (vec.empty()) return MPI_GROUP_EMPTY;
    int idx = groups.size();
    groups.resize(idx+1);
    groups[idx]=new groupStruct(vec);
    return (MPI_Group)idx;
  }
  inline int getRank(const MPI_Group group) const noexcept {
    vector<int> vec = group2vec(group);
    return getPosOp(thisIndex,vec);
  }
  inline AmpiRequestList &getReqs() noexcept { return ampiReqs; }
  inline int getMyPe() const noexcept {
    return CkMyPe();
  }
  inline bool hasWorld() const noexcept {
    return worldPtr!=NULL;
  }

  inline void checkComm(MPI_Comm comm) const noexcept {
    if ((comm != MPI_COMM_SELF && comm != MPI_COMM_WORLD)
     || (isSplit(comm) && comm-MPI_COMM_FIRST_SPLIT >= splitComm.size())
     || (isGroup(comm) && comm-MPI_COMM_FIRST_GROUP >= groupComm.size())
     || (isCart(comm)  && comm-MPI_COMM_FIRST_CART  >=  cartComm.size())
     || (isGraph(comm) && comm-MPI_COMM_FIRST_GRAPH >= graphComm.size())
     || (isDistGraph(comm) && comm-MPI_COMM_FIRST_DIST_GRAPH >= distGraphComm.size())
     || (isInter(comm) && comm-MPI_COMM_FIRST_INTER >= interComm.size())
     || (isIntra(comm) && comm-MPI_COMM_FIRST_INTRA >= intraComm.size()) )
      CkAbort("Invalid MPI_Comm\n");
  }

  /// if intra-communicator, return comm, otherwise return null group
  inline MPI_Group comm2group(const MPI_Comm comm) const noexcept {
    if(isInter(comm)) return MPI_GROUP_NULL;   // we don't support inter-communicator in such functions
    ampiCommStruct s = comm2CommStruct(comm);
    if(comm!=MPI_COMM_WORLD && comm!=s.getComm()) CkAbort("Error in ampiParent::comm2group()");
    return (MPI_Group)(s.getComm());
  }

  inline int getRemoteSize(const MPI_Comm comm) const noexcept {
    if(isInter(comm)) return getInter(comm).getRemoteIndices().size();
    else return -1;
  }
  inline MPI_Group getRemoteGroup(const MPI_Comm comm) noexcept {
    if(isInter(comm)) return saveGroupStruct(getInter(comm).getRemoteIndices());
    else return MPI_GROUP_NULL;
  }

  int createKeyval(MPI_Copy_function *copy_fn, MPI_Delete_function *delete_fn,
                  int *keyval, void* extra_state) noexcept;
  bool getBuiltinKeyval(int keyval, void *attribute_val) noexcept;
  int setUserKeyval(MPI_Comm comm, int keyval, void *attribute_val) noexcept;
  bool getUserKeyval(MPI_Comm comm, vector<int>& keyvals, int keyval, void *attribute_val, int *flag) noexcept;
  int dupUserKeyvals(MPI_Comm old_comm, MPI_Comm new_comm) noexcept;
  int freeUserKeyval(int context, vector<int>& keyvals, int *keyval) noexcept;
  int freeUserKeyvals(int context, vector<int>& keyvals) noexcept;

  int setAttr(MPI_Comm comm, vector<int>& keyvals, int keyval, void *attribute_val) noexcept;
  int getAttr(MPI_Comm comm, vector<int>& keyvals, int keyval, void *attribute_val, int *flag) noexcept;
  int deleteAttr(MPI_Comm comm, vector<int>& keyvals, int keyval) noexcept;

  int addWinStruct(WinStruct *win) noexcept;
  WinStruct *getWinStruct(MPI_Win win) const noexcept;
  void removeWinStruct(WinStruct *win) noexcept;

  int createInfo(MPI_Info *newinfo) noexcept;
  int dupInfo(MPI_Info info, MPI_Info *newinfo) noexcept;
  int setInfo(MPI_Info info, const char *key, const char *value) noexcept;
  int deleteInfo(MPI_Info info, const char *key) noexcept;
  int getInfo(MPI_Info info, const char *key, int valuelen, char *value, int *flag) const noexcept;
  int getInfoValuelen(MPI_Info info, const char *key, int *valuelen, int *flag) const noexcept;
  int getInfoNkeys(MPI_Info info, int *nkeys) const noexcept;
  int getInfoNthkey(MPI_Info info, int n, char *key) const noexcept;
  int freeInfo(MPI_Info info) noexcept;
  void defineInfoEnv(int nRanks_) noexcept;
  void defineInfoMigration() noexcept;

  // An 'MPI_Op' is an integer that indexes into either:
  //   A) an array of predefined ops owned by ampiNodeMgr, or
  //   B) a vector of user-defined ops owned by ampiParent
  // The MPI_Op is compared to AMPI_MAX_PREDEFINED_OP to disambiguate.
  inline int createOp(MPI_User_function *fn, bool isCommutative) noexcept {
    // Search thru non-predefined op's for any invalidated ones:
    for (int i=0; i<userOps.size(); i++) {
      if (userOps[i].isFree()) {
        userOps[i].init(fn, isCommutative);
        return AMPI_MAX_PREDEFINED_OP + 1 + i;
      }
    }
    // No invalid entries, so create a new one:
    userOps.emplace_back(fn, isCommutative);
    return AMPI_MAX_PREDEFINED_OP + userOps.size();
  }
  inline void freeOp(MPI_Op op) noexcept {
    // Don't free predefined op's:
    if (!opIsPredefined(op)) {
      // Invalidate op, then free all invalid op's from the back of the userOp's vector
      int opIdx = op - 1 - AMPI_MAX_PREDEFINED_OP;
      CkAssert(opIdx < userOps.size());
      userOps[opIdx].free();
      while (!userOps.empty() && userOps.back().isFree()) {
        userOps.pop_back();
      }
    }
  }
  inline bool opIsPredefined(MPI_Op op) const noexcept {
    return (op <= AMPI_MAX_PREDEFINED_OP);
  }
  inline bool opIsCommutative(MPI_Op op) const noexcept {
    if (opIsPredefined(op)) {
      return true; // all predefined ops are commutative
    }
    else {
      int opIdx = op - 1 - AMPI_MAX_PREDEFINED_OP;
      CkAssert(opIdx < userOps.size());
      return userOps[opIdx].isCommutative;
    }
  }
  inline MPI_User_function* op2User_function(MPI_Op op) const noexcept {
    if (opIsPredefined(op)) {
      return predefinedOps[op];
    }
    else {
      int opIdx = op - 1 - AMPI_MAX_PREDEFINED_OP;
      CkAssert(opIdx < userOps.size());
      return userOps[opIdx].func;
    }
  }
  inline AmpiOpHeader op2AmpiOpHeader(MPI_Op op, MPI_Datatype type, int count) const noexcept {
    if (opIsPredefined(op)) {
      int size = myDDT.getType(type)->getSize(count);
      return AmpiOpHeader(predefinedOps[op], type, count, size);
    }
    else {
      int opIdx = op - 1 - AMPI_MAX_PREDEFINED_OP;
      CkAssert(opIdx < userOps.size());
      int size = myDDT.getType(type)->getSize(count);
      return AmpiOpHeader(userOps[opIdx].func, type, count, size);
    }
  }
  inline void applyOp(MPI_Datatype datatype, MPI_Op op, int count, const void* invec, void* inoutvec) const noexcept {
    // inoutvec[i] = invec[i] op inoutvec[i]
    MPI_User_function *func = op2User_function(op);
    (func)((void*)invec, inoutvec, &count, &datatype);
  }

  void init() noexcept;
  void finalize() noexcept;
  CMI_WARN_UNUSED_RESULT ampiParent* block() noexcept;
  CMI_WARN_UNUSED_RESULT ampiParent* yield() noexcept;

#if AMPI_PRINT_MSG_SIZES
// Map of AMPI routine names to message sizes and number of messages:
// ["AMPI_Routine"][ [msg_size][num_msgs] ]
  std::unordered_map<std::string, std::map<int, int> > msgSizes;
  inline bool isRankRecordingMsgSizes() noexcept;
  inline void recordMsgSize(const char* func, int msgSize) noexcept;
  void printMsgSizes() noexcept;
#endif

#if AMPIMSGLOG
  /* message logging */
  int pupBytes;
#if CMK_USE_ZLIB && 0
  gzFile fMsgLog;
  PUP::tozDisk *toPUPer;
  PUP::fromzDisk *fromPUPer;
#else
  FILE* fMsgLog;
  PUP::toDisk *toPUPer;
  PUP::fromDisk *fromPUPer;
#endif
#endif
};

// Store a generalized request class created by MPIX_Grequest_class_create
class greq_class_desc {
public:
  MPI_Grequest_query_function *query_fn;
  MPI_Grequest_free_function *free_fn;
  MPI_Grequest_cancel_function *cancel_fn;
  MPIX_Grequest_poll_function *poll_fn;
  MPIX_Grequest_wait_function *wait_fn;

  void pup(PUP::er &p) noexcept {
    p((char *)query_fn, sizeof(void *));
    p((char *)free_fn, sizeof(void *));
    p((char *)cancel_fn, sizeof(void *));
    p((char *)poll_fn, sizeof(void *));
    p((char *)wait_fn, sizeof(void *));
  }
};

/*
An ampi manages the communication of one thread over
one MPI communicator.
*/
class ampi final : public CBase_ampi {
 private:
  friend class IReq; // for checking resumeOnRecv
  friend class SendReq;
  friend class SsendReq;
  friend class RednReq;
  friend class GatherReq;
  friend class GathervReq;

  ampiParent *parent;
  CProxy_ampiParent parentProxy;
  TCharm *thread;
  int myRank;
  AmpiSeqQ oorder;

 public:
  /*
   * AMPI Message Matching (Amm) queues are indexed by the tag and sender.
   * Since ampi objects are per-communicator, there are separate Amm's per communicator.
   */
  Amm<AmpiRequest *, AMPI_AMM_PT2PT_POOL_SIZE> postedReqs;
  Amm<AmpiMsg *, AMPI_AMM_PT2PT_POOL_SIZE> unexpectedMsgs;

  // Bcast requests / msgs must be kept separate from pt2pt,
  // so we don't match them to wildcard recv's
  Amm<AmpiRequest *, AMPI_AMM_COLL_POOL_SIZE> postedBcastReqs;
  Amm<AmpiMsg *, AMPI_AMM_COLL_POOL_SIZE> unexpectedBcastMsgs;

  // Store generalized request classes created by MPIX_Grequest_class_create
  vector<greq_class_desc> greq_classes;

 private:
  ampiCommStruct myComm;
  vector<int> tmpVec; // stores temp group info
  CProxy_ampi remoteProxy; // valid only for intercommunicator
  CkPupPtrVec<win_obj> winObjects;

 private:
  inline bool isInOrder(int seqIdx, int seq) noexcept { return oorder.isInOrder(seqIdx, seq); }
  bool inorder(AmpiMsg *msg) noexcept;
  void inorderBcast(AmpiMsg *msg, bool deleteMsg) noexcept;
  void inorderRdma(char* buf, int size, CMK_REFNUM_TYPE seq, int tag, int srcRank) noexcept;
  inline void localInorder(char* buf, int size, int seqIdx, CMK_REFNUM_TYPE seq, int tag,
                           int srcRank, IReq* ireq) noexcept;

  void init() noexcept;
  void findParent(bool forMigration) noexcept;

 public: // entry methods
  ampi() noexcept;
  ampi(CkArrayID parent_,const ampiCommStruct &s) noexcept;
  ampi(CkMigrateMessage *msg) noexcept;
  void ckJustMigrated() noexcept;
  void ckJustRestored() noexcept;
  ~ampi() noexcept;

  void pup(PUP::er &p) noexcept;

  void allInitDone() noexcept;
  void setInitDoneFlag() noexcept;

  inline void unblock() noexcept {
    thread->resume();
  }

  void injectMsg(int size, char* buf) noexcept;
  void genericSync(AmpiMsg *) noexcept;
  void generic(AmpiMsg *) noexcept;
  void genericRdma(char* buf, int size, CMK_REFNUM_TYPE seq, int tag, int srcRank) noexcept;
  void completedRdmaSend(CkDataMsg *msg) noexcept;
  void ssendAck(int sreqIdx) noexcept;
  void bcastResult(AmpiMsg *msg) noexcept;
  void barrierResult(void) noexcept;
  void ibarrierResult(void) noexcept;
  void rednResult(CkReductionMsg *msg) noexcept;
  void irednResult(CkReductionMsg *msg) noexcept;

  void splitPhase1(CkReductionMsg *msg) noexcept;
  void splitPhaseInter(CkReductionMsg *msg) noexcept;
  void commCreatePhase1(MPI_Comm nextGroupComm) noexcept;
  void intercommCreatePhase1(MPI_Comm nextInterComm) noexcept;
  void intercommMergePhase1(MPI_Comm nextIntraComm) noexcept;

 private: // Used by the above entry methods that create new MPI_Comm objects
  CProxy_ampi createNewChildAmpiSync() noexcept;
  void insertNewChildAmpiElements(MPI_Comm newComm, CProxy_ampi newAmpi) noexcept;

  inline void handleBlockedReq(AmpiRequest* req) noexcept {
    if (req->isBlocked() && parent->numBlockedReqs != 0) {
      parent->numBlockedReqs--;
    }
  }
  inline void resumeThreadIfReady() noexcept {
    if (parent->resumeOnRecv && parent->numBlockedReqs == 0) {
      thread->resume();
    }
  }

 private: // for this pointer safety after migration
  CMI_WARN_UNUSED_RESULT static ampi* static_blockOnColl(ampi* dis) noexcept;
  static int static_recv(ampi* dis,int t,int s,void* buf,int count,MPI_Datatype type,MPI_Comm comm,MPI_Status *sts) noexcept;
  static void static_probe(ampi* dis,int t,int s,MPI_Comm comm,MPI_Status *sts) noexcept;
  static void static_mprobe(ampi* dis, int t, int s, MPI_Comm comm, MPI_Status *sts, MPI_Message *message) noexcept;

 public: // to be used by MPI_* functions
  inline const ampiCommStruct &comm2CommStruct(MPI_Comm comm) const noexcept {
    return parent->comm2CommStruct(comm);
  }
  inline const ampiCommStruct &getCommStruct() const noexcept { return myComm; }

  CMI_WARN_UNUSED_RESULT inline ampi* blockOnRecv() noexcept;
  CMI_WARN_UNUSED_RESULT CMI_FORCE_INLINE ampi* blockOnColl() noexcept {
    return static_blockOnColl(this);
  }
  inline void setBlockingReq(AmpiRequest *req) noexcept;
  CMI_WARN_UNUSED_RESULT inline ampi* blockOnIReq(void* buf, int count, MPI_Datatype type, int s,
                                                  int t, MPI_Comm comm, MPI_Status* sts) noexcept;
  MPI_Request postReq(AmpiRequest* newreq) noexcept;
  inline void waitOnBlockingSend(MPI_Request* req, AmpiSendType sendType) noexcept;
  inline void requestSsendMsg(AmpiMsg* msg) noexcept;
  inline void completedSend(MPI_Request req) noexcept;

  inline CMK_REFNUM_TYPE getSeqNo(int destRank, MPI_Comm destcomm, int tag) noexcept;
  AmpiMsg *makeBcastMsg(const void *buf,int count,MPI_Datatype type,int root,MPI_Comm destcomm) noexcept;
  AmpiMsg *makeSyncMsg(int destRank,int t,int sRank,const void *buf,int count,
                       MPI_Datatype type,MPI_Comm destcomm,int ssendReq,CMK_REFNUM_TYPE seq) noexcept;
  AmpiMsg *makeAmpiMsg(int destRank,int t,int sRank,const void *buf,int count,
                       MPI_Datatype type,MPI_Comm destcomm) noexcept;
  AmpiMsg *makeAmpiMsg(int destRank,int t,int sRank,const void *buf,int count,
                       MPI_Datatype type,MPI_Comm destcomm,CMK_REFNUM_TYPE seq) noexcept;

  MPI_Request send(int t, int s, const void* buf, int count, MPI_Datatype type, int rank,
                   MPI_Comm destcomm, AmpiSendType sendType=BLOCKING_SEND, MPI_Request=MPI_REQUEST_NULL) noexcept;
  static void sendraw(int t, int s, void* buf, int len, CkArrayID aid, int idx) noexcept;
  inline MPI_Request sendSyncMsg(int t, int sRank, const void* buf, MPI_Datatype type, int count,
                                 int rank, MPI_Comm destcomm, CMK_REFNUM_TYPE seq, CProxyElement_ampi destElem,
                                 AmpiSendType sendType, MPI_Request reqIdx) noexcept;
  inline MPI_Request sendLocalMsg(int t, int sRank, const void* buf, int size, MPI_Datatype type,
                                  int count, int destRank, MPI_Comm destcomm, CMK_REFNUM_TYPE seq,
                                  ampi* destPtr, AmpiSendType sendType, MPI_Request reqIdx) noexcept;
  inline MPI_Request sendRdmaMsg(int t, int sRank, const void* buf, int size, MPI_Datatype type, int destIdx,
                                 int destRank, MPI_Comm destcomm, CMK_REFNUM_TYPE seq, CProxy_ampi arrProxy,
                                 MPI_Request reqIdx) noexcept;
  inline bool destLikelyWithinProcess(CProxy_ampi arrProxy, int destIdx) const noexcept {
#if CMK_MULTICORE
    return true;
#elif CMK_SMP
    CkArray* localBranch = arrProxy.ckLocalBranch();
    int destPe = localBranch->lastKnown(CkArrayIndex1D(destIdx));
    return (CkNodeOf(destPe) == CkMyNode());
#else // non-SMP
    ampi* destPtr = arrProxy[destIdx].ckLocal();
    return (destPtr != NULL);
#endif
  }
  inline MPI_Request delesend(int t, int s, const void* buf, int count, MPI_Datatype type, int rank,
                              MPI_Comm destcomm, CProxy_ampi arrproxy, AmpiSendType sendType, MPI_Request req) noexcept;
  inline bool processSsendMsg(AmpiMsg* msg, int* msgLen, char** msgData) noexcept;
  inline bool processAmpiMsg(AmpiMsg *msg, void* buf, MPI_Datatype type, int count) noexcept;
  inline void processRdmaMsg(const void *sbuf, int slength, int srank, void* rbuf,
                             int rcount, MPI_Datatype rtype, MPI_Comm comm) noexcept;
  inline void processRednMsg(CkReductionMsg *msg, void* buf, MPI_Datatype type, int count) noexcept;
  inline void processNoncommutativeRednMsg(CkReductionMsg *msg, void* buf, MPI_Datatype type, int count,
                                           MPI_User_function* func) noexcept;
  inline void processGatherMsg(CkReductionMsg *msg, void* buf, MPI_Datatype type, int recvCount) noexcept;
  inline void processGathervMsg(CkReductionMsg *msg, void* buf, MPI_Datatype type,
                               int* recvCounts, int* displs) noexcept;
  inline AmpiMsg * getMessage(int t, int s, MPI_Comm comm, int *sts) const noexcept;
  CMI_FORCE_INLINE int recv(int t,int s,void* buf,int count,MPI_Datatype type,MPI_Comm comm,MPI_Status *sts=NULL) noexcept {
    return static_recv(this, t, s, buf, count, type, comm, sts);
  }
  void irecv(void *buf, int count, MPI_Datatype type, int src,
             int tag, MPI_Comm comm, MPI_Request *request) noexcept;
  void mrecv(int tag, int src, void* buf, int count, MPI_Datatype datatype, MPI_Comm comm,
             MPI_Status* status, MPI_Message* message) noexcept;
  void imrecv(void* buf, int count, MPI_Datatype datatype, int src, int tag, MPI_Comm comm,
              MPI_Request* request, MPI_Message* message) noexcept;
  void irecvBcast(void *buf, int count, MPI_Datatype type, int src,
                  MPI_Comm comm, MPI_Request *request) noexcept;
  void sendrecv(const void *sbuf, int scount, MPI_Datatype stype, int dest, int stag,
                void *rbuf, int rcount, MPI_Datatype rtype, int src, int rtag,
                MPI_Comm comm, MPI_Status *sts) noexcept;
  void sendrecv_replace(void* buf, int count, MPI_Datatype datatype,
                        int dest, int sendtag, int source, int recvtag,
                        MPI_Comm comm, MPI_Status *status) noexcept;
  CMI_FORCE_INLINE void probe(int t,int s,MPI_Comm comm,MPI_Status *sts) noexcept {
    return static_probe(this, t, s, comm, sts);
  }
  CMI_FORCE_INLINE void mprobe(int t, int s, MPI_Comm comm, MPI_Status *sts, MPI_Message *message) noexcept {
    return static_mprobe(this, t, s, comm, sts, message);
  }
  int iprobe(int t,int s,MPI_Comm comm,MPI_Status *sts) noexcept;
  int improbe(int t, int s, MPI_Comm comm, MPI_Status *sts, MPI_Message *message) noexcept;
  CMI_WARN_UNUSED_RESULT ampi * barrier() noexcept;
  CMI_WARN_UNUSED_RESULT ampi * block() noexcept;
  CMI_WARN_UNUSED_RESULT ampi * yield() noexcept;
  void ibarrier(MPI_Request *request) noexcept;
  void bcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm comm) noexcept;
  int intercomm_bcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm intercomm) noexcept;
  void ibcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm comm, MPI_Request* request) noexcept;
  int intercomm_ibcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm intercomm, MPI_Request *request) noexcept;
  static void bcastraw(void* buf, int len, CkArrayID aid) noexcept;
  void split(int color,int key,MPI_Comm *dest, int type) noexcept;
  void commCreate(const vector<int>& vec,MPI_Comm *newcomm) noexcept;
  MPI_Comm cartCreate0D() noexcept;
  MPI_Comm cartCreate(vector<int>& vec, int ndims, const int* dims) noexcept;
  void graphCreate(const vector<int>& vec, MPI_Comm *newcomm) noexcept;
  void distGraphCreate(const vector<int>& vec, MPI_Comm *newcomm) noexcept;
  void intercommCreate(const vector<int>& rvec, int root, MPI_Comm tcomm, MPI_Comm *ncomm) noexcept;

  inline bool isInter() const noexcept { return myComm.isinter(); }
  void intercommMerge(int first, MPI_Comm *ncomm) noexcept;

  inline ampiParent* getParent() const noexcept { return parent; }
  inline int getWorldRank() const noexcept {return parent->thisIndex;}
  /// Return our rank in this communicator
  inline int getRank() const noexcept {return myRank;}
  inline int getSize() const noexcept {return myComm.getSize();}
  inline MPI_Comm getComm() const noexcept {return myComm.getComm();}
  inline void setCommName(const char *name) noexcept {myComm.setName(name);}
  inline void getCommName(char *name, int *len) const noexcept {myComm.getName(name,len);}
  inline vector<int> getIndices() const noexcept { return myComm.getIndices(); }
  inline vector<int> getRemoteIndices() const noexcept { return myComm.getRemoteIndices(); }
  inline const CProxy_ampi &getProxy() const noexcept {return thisProxy;}
  inline const CProxy_ampi &getRemoteProxy() const noexcept {return remoteProxy;}
  inline void setRemoteProxy(CProxy_ampi rproxy) noexcept { remoteProxy = rproxy; thread->resume(); }
  inline int getIndexForRank(int r) const noexcept {return myComm.getIndexForRank(r);}
  inline int getIndexForRemoteRank(int r) const noexcept {return myComm.getIndexForRemoteRank(r);}
  void findNeighbors(MPI_Comm comm, int rank, vector<int>& neighbors) const noexcept;
  inline const vector<int>& getNeighbors() const noexcept { return myComm.getTopologyforNeighbors()->getnbors(); }
  inline bool opIsCommutative(MPI_Op op) const noexcept { return parent->opIsCommutative(op); }
  inline MPI_User_function* op2User_function(MPI_Op op) const noexcept { return parent->op2User_function(op); }
  void topoDup(int topoType, int rank, MPI_Comm comm, MPI_Comm *newcomm) noexcept;

  inline AmpiRequestList& getReqs() noexcept { return parent->ampiReqs; }
  CkDDT *getDDT() noexcept {return &parent->myDDT;}
  CthThread getThread() const noexcept { return thread->getThread(); }

 public:
  MPI_Win createWinInstance(void *base, MPI_Aint size, int disp_unit, MPI_Info info) noexcept;
  int deleteWinInstance(MPI_Win win) noexcept;
  int winGetGroup(WinStruct *win, MPI_Group *group) const noexcept;
  int winPut(const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
             MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, WinStruct *win) noexcept;
  int winGet(void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
             MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, WinStruct *win) noexcept;
  int winIget(MPI_Aint orgdisp, int orgcnt, MPI_Datatype orgtype, int rank,
              MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, WinStruct *win,
              MPI_Request *req) noexcept;
  int winIgetWait(MPI_Request *request, MPI_Status *status) noexcept;
  int winIgetFree(MPI_Request *request, MPI_Status *status) noexcept;
  void winRemotePut(int orgtotalsize, char* orgaddr, int orgcnt, MPI_Datatype orgtype,
                    MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, int winIndex) noexcept;
  char* winLocalGet(int orgcnt, MPI_Datatype orgtype, MPI_Aint targdisp, int targcnt,
                    MPI_Datatype targtype, int winIndex) noexcept;
  AmpiMsg* winRemoteGet(int orgcnt, MPI_Datatype orgtype, MPI_Aint targdisp,
                    int targcnt, MPI_Datatype targtype, int winIndex) noexcept;
  AmpiMsg* winRemoteIget(MPI_Aint orgdisp, int orgcnt, MPI_Datatype orgtype, MPI_Aint targdisp,
                         int targcnt, MPI_Datatype targtype, int winIndex) noexcept;
  int winLock(int lock_type, int rank, WinStruct *win) noexcept;
  int winUnlock(int rank, WinStruct *win) noexcept;
  void winRemoteLock(int lock_type, int winIndex, int requestRank) noexcept;
  void winRemoteUnlock(int winIndex, int requestRank) noexcept;
  int winAccumulate(const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
                    MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                    MPI_Op op, WinStruct *win) noexcept;
  void winRemoteAccumulate(int orgtotalsize, char* orgaddr, int orgcnt, MPI_Datatype orgtype,
                           MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                           MPI_Op op, int winIndex) noexcept;
  int winGetAccumulate(const void *orgaddr, int orgcnt, MPI_Datatype orgtype, void *resaddr,
                       int rescnt, MPI_Datatype restype, int rank, MPI_Aint targdisp,
                       int targcnt, MPI_Datatype targtype, MPI_Op op, WinStruct *win) noexcept;
  void winLocalGetAccumulate(int orgtotalsize, char* sorgaddr, int orgcnt, MPI_Datatype orgtype,
                             MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Op op,
                             char *resaddr, int winIndex) noexcept;
  AmpiMsg* winRemoteGetAccumulate(int orgtotalsize, char* sorgaddr, int orgcnt, MPI_Datatype orgtype,
                                  MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Op op,
                                  int winIndex) noexcept;
  int winCompareAndSwap(const void *orgaddr, const void *compaddr, void *resaddr, MPI_Datatype type,
                        int rank, MPI_Aint targdisp, WinStruct *win) noexcept;
  char* winLocalCompareAndSwap(int size, char* sorgaddr, char* compaddr, MPI_Datatype type,
                               MPI_Aint targdisp, int winIndex) noexcept;
  AmpiMsg* winRemoteCompareAndSwap(int size, char *sorgaddr, char *compaddr, MPI_Datatype type,
                                   MPI_Aint targdisp, int winIndex) noexcept;
  void winSetName(WinStruct *win, const char *name) noexcept;
  void winGetName(WinStruct *win, char *name, int *length) const noexcept;
  win_obj* getWinObjInstance(WinStruct *win) const noexcept;
  int getNewSemaId() noexcept;

  int intercomm_scatter(int root, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                        void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm intercomm) noexcept;
  int intercomm_iscatter(int root, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                         void *recvbuf, int recvcount, MPI_Datatype recvtype,
                         MPI_Comm intercomm, MPI_Request *request) noexcept;
  int intercomm_scatterv(int root, const void* sendbuf, const int* sendcounts, const int* displs,
                         MPI_Datatype sendtype, void* recvbuf, int recvcount,
                         MPI_Datatype recvtype, MPI_Comm intercomm) noexcept;
  int intercomm_iscatterv(int root, const void* sendbuf, const int* sendcounts, const int* displs,
                          MPI_Datatype sendtype, void* recvbuf, int recvcount,
                          MPI_Datatype recvtype, MPI_Comm intercomm, MPI_Request* request) noexcept;
};

CMI_WARN_UNUSED_RESULT ampiParent *getAmpiParent() noexcept;
bool isAmpiThread() noexcept;
CMI_WARN_UNUSED_RESULT ampi *getAmpiInstance(MPI_Comm comm) noexcept;
void checkComm(MPI_Comm comm) noexcept;
void checkRequest(MPI_Request req) noexcept;
void handle_MPI_BOTTOM(void* &buf, MPI_Datatype type) noexcept;
void handle_MPI_BOTTOM(void* &buf1, MPI_Datatype type1, void* &buf2, MPI_Datatype type2) noexcept;

#if AMPI_ERROR_CHECKING
int ampiErrhandler(const char* func, int errcode) noexcept;
#else
#define ampiErrhandler(func, errcode) (errcode)
#endif


#if CMK_TRACE_ENABLED

// List of AMPI functions to trace:
static const char *funclist[] = {"AMPI_Abort", "AMPI_Add_error_class", "AMPI_Add_error_code", "AMPI_Add_error_string",
"AMPI_Address", "AMPI_Allgather", "AMPI_Allgatherv", "AMPI_Allreduce", "AMPI_Alltoall",
"AMPI_Alltoallv", "AMPI_Alltoallw", "AMPI_Attr_delete", "AMPI_Attr_get",
"AMPI_Attr_put", "AMPI_Barrier", "AMPI_Bcast", "AMPI_Bsend", "AMPI_Cancel",
"AMPI_Cart_coords", "AMPI_Cart_create", "AMPI_Cart_get", "AMPI_Cart_map",
"AMPI_Cart_rank", "AMPI_Cart_shift", "AMPI_Cart_sub", "AMPI_Cartdim_get",
"AMPI_Comm_call_errhandler", "AMPI_Comm_compare", "AMPI_Comm_create", "AMPI_Comm_create_group",
"AMPI_Comm_create_errhandler", "AMPI_Comm_create_keyval", "AMPI_Comm_delete_attr",
"AMPI_Comm_dup", "AMPI_Comm_dup_with_info", "AMPI_Comm_free",
"AMPI_Comm_free_errhandler", "AMPI_Comm_free_keyval", "AMPI_Comm_get_attr",
"AMPI_Comm_get_errhandler", "AMPI_Comm_get_info", "AMPI_Comm_get_name",
"AMPI_Comm_group", "AMPI_Comm_rank", "AMPI_Comm_remote_group", "AMPI_Comm_remote_size",
"AMPI_Comm_set_attr", "AMPI_Comm_set_errhandler", "AMPI_Comm_set_info", "AMPI_Comm_set_name",
"AMPI_Comm_size", "AMPI_Comm_split", "AMPI_Comm_split_type", "AMPI_Comm_test_inter",
"AMPI_Dims_create", "AMPI_Dist_graph_create", "AMPI_Dist_graph_create_adjacent",
"AMPI_Dist_graph_neighbors", "AMPI_Dist_graph_neighbors_count",
"AMPI_Errhandler_create", "AMPI_Errhandler_free", "AMPI_Errhandler_get",
"AMPI_Errhandler_set", "AMPI_Error_class", "AMPI_Error_string", "AMPI_Exscan", "AMPI_Finalize",
"AMPI_Finalized", "AMPI_Gather", "AMPI_Gatherv", "AMPI_Get_address", "AMPI_Get_count",
"AMPI_Get_elements", "AMPI_Get_library_version", "AMPI_Get_processor_name", "AMPI_Get_version",
"AMPI_Graph_create", "AMPI_Graph_get", "AMPI_Graph_map", "AMPI_Graph_neighbors",
"AMPI_Graph_neighbors_count", "AMPI_Graphdims_get", "AMPI_Group_compare", "AMPI_Group_difference",
"AMPI_Group_excl", "AMPI_Group_free", "AMPI_Group_incl", "AMPI_Group_intersection",
"AMPI_Group_range_excl", "AMPI_Group_range_incl", "AMPI_Group_rank", "AMPI_Group_size",
"AMPI_Group_translate_ranks", "AMPI_Group_union", "AMPI_Iallgather", "AMPI_Iallgatherv",
"AMPI_Iallreduce", "AMPI_Ialltoall", "AMPI_Ialltoallv", "AMPI_Ialltoallw", "AMPI_Ibarrier",
"AMPI_Ibcast", "AMPI_Iexscan", "AMPI_Igather", "AMPI_Igatherv", "AMPI_Ineighbor_allgather",
"AMPI_Ineighbor_allgatherv", "AMPI_Ineighbor_alltoall", "AMPI_Ineighbor_alltoallv",
"AMPI_Ineighbor_alltoallw", "AMPI_Init", "AMPI_Init_thread", "AMPI_Initialized", "AMPI_Intercomm_create",
"AMPI_Intercomm_merge", "AMPI_Iprobe", "AMPI_Irecv", "AMPI_Ireduce", "AMPI_Ireduce_scatter",
"AMPI_Ireduce_scatter_block", "AMPI_Is_thread_main", "AMPI_Iscan", "AMPI_Iscatter", "AMPI_Iscatterv",
"AMPI_Isend", "AMPI_Issend", "AMPI_Keyval_create", "AMPI_Keyval_free", "AMPI_Neighbor_allgather",
"AMPI_Neighbor_allgatherv", "AMPI_Neighbor_alltoall", "AMPI_Neighbor_alltoallv", "AMPI_Neighbor_alltoallw",
"AMPI_Op_commutative", "AMPI_Op_create", "AMPI_Op_free", "AMPI_Pack", "AMPI_Pack_size",
"AMPI_Pcontrol", "AMPI_Probe", "AMPI_Query_thread", "AMPI_Recv", "AMPI_Recv_init", "AMPI_Reduce",
"AMPI_Reduce_local", "AMPI_Reduce_scatter", "AMPI_Reduce_scatter_block", "AMPI_Request_free",
"AMPI_Request_get_status", "AMPI_Rsend", "AMPI_Scan", "AMPI_Scatter", "AMPI_Scatterv", "AMPI_Send",
"AMPI_Send_init",  "AMPI_Sendrecv", "AMPI_Sendrecv_replace", "AMPI_Ssend", "AMPI_Ssend_init",
"AMPI_Start", "AMPI_Startall", "AMPI_Status_set_cancelled", "AMPI_Status_set_elements", "AMPI_Test",
"AMPI_Test_cancelled", "AMPI_Testall", "AMPI_Testany", "AMPI_Testsome", "AMPI_Topo_test",
"AMPI_Type_commit", "AMPI_Type_contiguous", "AMPI_Type_create_hindexed",
"AMPI_Type_create_hindexed_block", "AMPI_Type_create_hvector", "AMPI_Type_create_indexed_block",
"AMPI_Type_create_keyval", "AMPI_Type_create_resized", "AMPI_Type_create_struct",
"AMPI_Type_delete_attr", "AMPI_Type_dup", "AMPI_Type_extent", "AMPI_Type_free",
"AMPI_Type_free_keyval", "AMPI_Type_get_attr", "AMPI_Type_get_contents", "AMPI_Type_get_envelope",
"AMPI_Type_get_extent", "AMPI_Type_get_name", "AMPI_Type_get_true_extent", "AMPI_Type_hindexed",
"AMPI_Type_hvector", "AMPI_Type_indexed", "AMPI_Type_lb", "AMPI_Type_set_attr",
"AMPI_Type_set_name", "AMPI_Type_size", "AMPI_Type_struct", "AMPI_Type_ub", "AMPI_Type_vector",
"AMPI_Type_create_darray", "AMPI_Type_create_subarray",
"AMPI_Unpack", "AMPI_Wait", "AMPI_Waitall", "AMPI_Waitany", "AMPI_Waitsome", "AMPI_Wtick", "AMPI_Wtime",
"AMPI_Accumulate", "AMPI_Compare_and_swap", "AMPI_Fetch_and_op", "AMPI_Get", "AMPI_Get_accumulate",
"AMPI_Info_create", "AMPI_Info_delete", "AMPI_Info_dup", "AMPI_Info_free", "AMPI_Info_get",
"AMPI_Info_get_nkeys", "AMPI_Info_get_nthkey", "AMPI_Info_get_valuelen",
"AMPI_Info_set", "AMPI_Put", "AMPI_Raccumulate", "AMPI_Rget", "AMPI_Rget_accumulate",
"AMPI_Rput", "AMPI_Win_complete", "AMPI_Win_create", "AMPI_Win_create_errhandler",
"AMPI_Win_create_keyval", "AMPI_Win_delete_attr", "AMPI_Win_fence", "AMPI_Win_free",
"AMPI_Win_free_keyval", "AMPI_Win_get_attr", "AMPI_Win_get_errhandler",
"AMPI_Win_get_group", "AMPI_Win_get_info", "AMPI_Win_get_name", "AMPI_Win_lock",
"AMPI_Win_post", "AMPI_Win_set_attr", "AMPI_Win_set_errhandler", "AMPI_Win_set_info",
"AMPI_Win_set_name", "AMPI_Win_start", "AMPI_Win_test", "AMPI_Win_unlock",
"AMPI_Win_wait", "AMPI_Exit" /*AMPI extensions:*/, "AMPI_Migrate",
"AMPI_Load_start_measure", "AMPI_Load_stop_measure",
"AMPI_Load_set_value", "AMPI_Migrate_to_pe", "AMPI_Set_migratable",
"AMPI_Register_pup", "AMPI_Get_pup_data", "AMPI_Register_main",
"AMPI_Register_about_to_migrate", "AMPI_Register_just_migrated",
"AMPI_Iget", "AMPI_Iget_wait", "AMPI_Iget_free", "AMPI_Iget_data",
"AMPI_Type_is_contiguous", "AMPI_Yield", "AMPI_Suspend",
"AMPI_Resume", "AMPI_Print", "AMPI_Alltoall_medium",
"AMPI_Alltoall_long", "AMPI_System"};

// not traced: AMPI_Trace_begin, AMPI_Trace_end

#endif // CMK_TRACE_ENABLED

//Use this to mark the start of AMPI interface routines that can only be called on AMPI threads:
#if CMK_ERROR_CHECKING
#define AMPI_API(routineName) \
  if (!isAmpiThread()) { CkAbort("AMPI> cannot call MPI routines from non-AMPI threads!"); } \
  TCHARM_API_TRACE(routineName, "ampi");
#else
#define AMPI_API(routineName) TCHARM_API_TRACE(routineName, "ampi")
#endif

//Use this for MPI_Init and routines than can be called before AMPI threads have been initialized:
#define AMPI_API_INIT(routineName) TCHARM_API_TRACE(routineName, "ampi")

#endif // _AMPIIMPL_H
