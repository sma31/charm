#ifndef _CKDIRECT_H_
#define _CKDIRECT_H_
#include "cmidirect.h"
#include "charm++.h"

#define CkDirect_createHandle CmiDirect_createHandle
#define CkDirect_assocLocalBuffer CmiDirect_assocLocalBuffer
#define CkDirect_put CmiDirect_put
#define CkDirect_ready CmiDirect_ready

PUPbytes(infiDirectUserHandle)

#endif
