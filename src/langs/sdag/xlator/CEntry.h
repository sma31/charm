/*****************************************************************************
 * $Source$
 * $Author$
 * $Date$
 * $Revision$
 *****************************************************************************/

#ifndef _CEntry_H_
#define _CEntry_H_

#include "xi-util.h"
#include "sdag-globals.h"
#include "CList.h"

class CParseNode;

class CEntry{
  public:
    XStr *entry;
    XStr *msgType;
    int entryNum;
    int refNumNeeded;
    TList *whenList;
    CEntry(XStr *e, XStr *m) : entry(e), msgType(m) {
      entryNum = numEntries++;
      whenList = new TList();
      refNumNeeded=0;
    }
    void print(int indent) {
      Indent(indent);
      printf("entry %s (%s *)", entry->charstar(), msgType->charstar());
    }
    void generateCode(XStr *);
    void generateDeps(void);
    
};
#endif
