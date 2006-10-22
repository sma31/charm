#ifndef _GRIDCOMMLB_H_
#define _GRIDCOMMLB_H_

#include <limits.h>
#include <stdio.h>

#include "charm++.h"
#include "cklists.h"

#include "CentralLB.h"

#ifndef MAXINT
#define MAXINT 2147483647
#endif

#if CONVERSE_VERSION_VMI
extern "C" int CmiGetCluster (int process);
#endif

void CreateGridCommLB ();

class PE_Data_T
{
  public:
    CmiBool available;
    int cluster;
    int num_objs;
    int num_lan_objs;
    int num_lan_msgs;
    int num_wan_objs;
    int num_wan_msgs;
    double relative_speed;
    double scaled_load;
};

class Object_Data_T
{
  public:
    CmiBool migratable;
    int cluster;
    int from_pe;
    int to_pe;
    int num_lan_msgs;
    int num_wan_msgs;
    double load;
};

class GridCommLB : public CentralLB
{
  public:
    GridCommLB (const CkLBOptions &);
    GridCommLB (CkMigrateMessage *msg);

    CmiBool QueryBalanceNow (int step);
    void work (CentralLB::LDStats *stats, int count);
    void pup (PUP::er &p) { CentralLB::pup (p); }

  private:
    int Get_Cluster (int pe);
    void Initialize_PE_Data (CentralLB::LDStats *stats);
    int Available_PE_Count ();
    int Compute_Number_Of_Clusters ();
    void Initialize_Object_Data (CentralLB::LDStats *stats);
    void Examine_InterObject_Messages (CentralLB::LDStats *stats);
    void Map_NonMigratable_Objects_To_PEs ();
    void Map_Migratable_Objects_To_PEs (int cluster);
    int Find_Maximum_WAN_Object (int cluster);
    int Find_Minimum_WAN_PE (int cluster);
    void Assign_Object_To_PE (int target_object, int target_pe);

    int Num_PEs;
    int Num_Objects;
    int Num_Clusters;
    PE_Data_T *PE_Data;
    Object_Data_T *Object_Data;
};

#endif
