/*****************************************************************************
 * $Source$
 * $Author$
 * $Date$
 * $Revision$
 *****************************************************************************/

/**
 * \addtogroup CkLdb
*/
/*@{*/

#ifndef HYBRIDLB_H
#define HYBRIDLB_H

#include "charm++.h"
#include "BaseLB.h"
#include "CentralLB.h"
#include "HybridLB.decl.h"

#include "topology.h"

void CreateHybridLB();

/// for backward compatibility
typedef LBMigrateMsg NLBMigrateMsg;

inline int mymin(int x, int y) { return x<y?x:y; }

// base class
class MyHierarchyTree {
public:
  MyHierarchyTree() {}
  virtual ~MyHierarchyTree() {}
  virtual int numLevels() = 0;
  virtual int parent(int mype, int level) = 0;
  virtual int isroot(int mype, int level) = 0;
  virtual int numChildren(int mype, int level) = 0;
  virtual void getChildren(int mype, int level, int *children, int &count) = 0;
};

// a simple 3 layer tree, fat at level 1
//        1
//     ---+---
//     0  4  8
//  ---+--
//  0 1 2 3
class ThreeLevelTree: public MyHierarchyTree {
private:
  int span[2];
  int toproot;
  int nLevels;
public:
  ThreeLevelTree() {
    nLevels = 3;
    span[0] = CkNumPes()/8;
    CmiAssert(span[0]>0);
    span[1] = (CkNumPes()+span[0]-1)/span[0];
    toproot = 1;
  }
  virtual ~ThreeLevelTree() {}
  virtual int numLevels() { return nLevels; }
  virtual int parent(int mype, int level) {
    if (level == 0) return mype/span[0]*span[0];
    if (level == 1) return toproot;
    if (level == 2) return -1;
  }
  virtual int isroot(int mype, int level) {
    if (level == 0) return 0;
    if (level == 1 && mype % span[0] == 0) return 1;
    if (level == 2 && mype == toproot) return 1;
    return 0;
  }
  virtual int numChildren(int mype, int level) {
    if (level == 0) return 0;
    if (level == 1) return mymin(CkNumPes(), mype+span[0]) - mype;
    if (level == 2) return span[1];
  }
  virtual void getChildren(int mype, int level, int *children, int &count) {
    CmiAssert(isroot(mype, level));
    count = numChildren(mype, level);
    if (count == 0) { return; }
    if (level == 1) {
      for (int i=0; i<count; i++) 
        children[i] = mype + i;
    }
    if (level == 2) {
      for (int i=0; i<count; i++) 
        children[i] = i*span[0];
    }
  }
};

class HybridLB : public BaseLB
{
public:
  HybridLB(const CkLBOptions &);
  HybridLB(CkMigrateMessage *m):BaseLB(m) {}
  ~HybridLB();

  static void staticAtSync(void*);
  void AtSync(void); // Everything is at the PE barrier
  void ProcessAtSync(void);

  void ReceiveStats(CkMarshalledCLBStatsMessage &m, int fromlevel); 
  void ResumeClients(CkReductionMsg *msg);
  void ResumeClients();
  void ReceiveMigration(LBMigrateMsg *); 	// Receive migration data

  // Migrated-element callback
  static void staticMigrated(void* me, LDObjHandle h, int waitBarrier);
  void Migrated(LDObjHandle h, int waitBarrier);

  void ObjMigrated(LDObjData data, int level);
  void MigrationDone(int balancing);  // Call when migration is complete
  void StatsDone(int level);  // Call when LDStats migration is complete
  void NotifyObjectMigrationDone(int level);	
  void Loadbalancing(int level);	// start load balancing
  int step() { return mystep; };
  void StartCollectInfo();
  void CollectInfo(Location *loc, int n, int fromlevel);
  void PropagateInfo(Location *loc, int n, int fromlevel);

  struct MigrationRecord {
    LDObjHandle handle;
    int      fromPe;		// real from pe
    int      toPe;
    MigrationRecord(): fromPe(-1), toPe(-1) {}
    MigrationRecord(LDObjHandle &k, int f, int t): handle(k), fromPe(f), toPe(t) {}
    void pup(PUP::er &p) { p|handle; p|fromPe; p|toPe; }
  };

private:
  CProxy_HybridLB  thisProxy;
  int              foundNeighbors;
  int		   recvslot;

protected:
  virtual CmiBool QueryBalanceNow(int) { return CmiTrue; };  
  virtual CmiBool QueryMigrateStep(int) { return CmiTrue; };  
  virtual LBMigrateMsg* Strategy(LDStats* stats,int count);

  int NeighborIndex(int pe, int atlevel);   // return the neighbor array index

private:
  void BuildHierarchy();
  void FindNeighbors();
  CLBStatsMsg* AssembleStats();
  void buildStats(int level);
  CLBStatsMsg * buildCombinedLBStatsMessage(int atlevel);
  void depositLBStatsMessage(CLBStatsMsg *msg, int atlevel);

  int mystep;
  int future_migrates_expected;
  LBMigrateMsg** mig_msgs;
  int mig_msgs_received;
  int cur_ld_balancer;
  double start_lb_time;

  CentralLB *greedy;
  CentralLB *refine;

  MyHierarchyTree  *tree;

  class LevelData {
  public:
    int parent;
    int*  children;
    int nChildren;
    CLBStatsMsg **statsMsgsList;
    int stats_msg_count;
    LDStats *statsData;
    int obj_expected, obj_completed;
    int migrates_expected, migrates_completed;
    int mig_reported;
    int info_recved;
    int resumeAfterMigration;
    CkVec<MigrationRecord> outObjs;
    CkVec<Location> unmatchedObjs;
    CkVec<Location> matchedObjs;	 // don't need to be sent up
  public:
    LevelData(): parent(-1), children(NULL), nChildren(0), 
                 statsMsgsList(NULL), stats_msg_count(0),
                 statsData(NULL), obj_expected(-1), obj_completed(0),
		 migrates_expected(-1), migrates_completed(0),
                 mig_reported(0), info_recved(0), resumeAfterMigration(0)
 		 {}
    ~LevelData() {
      if (children) delete [] children;
      if (statsMsgsList) delete [] statsMsgsList;
      if (statsData) delete statsData;
    }
    int migrationDone() {
//CkPrintf("[%d] checking migrates_expected: %d migrates_completed: %d obj_completed: %d\n", migrates_expected, migrates_completed, obj_completed);
      return migrates_expected == 0 || migrates_completed + obj_completed == migrates_expected;
    }
    void clear() {
      obj_expected = -1;
      obj_completed = 0;
      migrates_expected = -1;
      migrates_completed = 0;
      mig_reported = 0;
      info_recved = 0;
      resumeAfterMigration = 0;
      if (statsData) statsData->clear();
      outObjs.free();
      matchedObjs.free();
      unmatchedObjs.free();
    }
  };

  CkVec<LevelData *>  levelData;

  int currentLevel;

  CkVec<Location> newObjs;

};

/*
class NLBStatsMsg {
public:
  int from_pe;
  int serial;
  int pe_speed;
  double total_walltime;
  double total_cputime;
  double idletime;
  double bg_walltime;
  double bg_cputime;
  double obj_walltime;   // may not needed
  double obj_cputime;   // may not needed
  int n_objs;
  LDObjData *objData;
  int n_comm;
  LDCommData *commData;
public:
  NLBStatsMsg(int osz, int csz);
  NLBStatsMsg(NLBStatsMsg *s);
  NLBStatsMsg()  {}
  ~NLBStatsMsg();
  void pup(PUP::er &p);
}; 
*/

#endif /* NBORBASELB_H */

/*@}*/
