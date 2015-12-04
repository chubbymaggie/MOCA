/*
 * Copyright (C) 2015  Diener, Matthias <mdiener@inf.ufrgs.br> and Beniamine, David <David@Beniamine.net>
 * Author: Beniamine, David <David@Beniamine.net>
 * Author: Diener, Matthias <mdiener@inf.ufrgs.br>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include <sstream>
#include <unordered_map>
#include <map>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <tuple>
#include <unistd.h>

#include <sys/time.h>
#include <sys/resource.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <errno.h>
#include <numa.h>
#include <numaif.h>

#include <libelf/libelf.h>
#include <libelf/gelf.h>
#include <execinfo.h>


#include "pin.H"

#define REAL_TID(tid) ((tid)>=2 ? (tid)-1 : (tid))
#define ACC_T_READ 0
#define ACC_T_WRITE 1
#define PAGE_MAPPING_CHECK_THRESHOLD 1000 // Check page mapping every 1000 acces by one thread to one page

char TYPE_NAME[2]={'R','W'};

const int MAXTHREADS = 1024;
int PAGESIZE;
unsigned int REAL_PAGESIZE;




struct{
    string sym;
    ADDRINT sz;
    int ended; // 0 if the malloc is not completed (missing ret val)
} Allocs[MAXTHREADS+1];

ofstream fstructStream;

int num_threads = 0;


array<UINT64, MAXTHREADS> stacksize; // stack size of each thread in pages
array<UINT64, MAXTHREADS> stackmax;  // tid -> stack base address from file (unpinned application)
map<UINT32, UINT64> stackmap;        // stack base address from pinned application

string img_name;




long GetStackSize()
{
    struct rlimit sl;
    int returnVal = getrlimit(RLIMIT_STACK, &sl);
    if (returnVal == -1)
    {
        cerr << "Error. errno: " << errno << endl;
    }
    return sl.rlim_cur;
}

VOID ThreadStart(THREADID tid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
    __sync_add_and_fetch(&num_threads, 1);

    if (num_threads>=MAXTHREADS+1) {
        cerr << "ERROR: num_threads (" << num_threads << ") higher than MAXTHREADS (" << MAXTHREADS << ")." << endl;
    }

    int pid = PIN_GetTid();
    stackmap[pid] = PIN_GetContextReg(ctxt, REG_STACK_PTR) >> PAGESIZE;
    // Print Tid, StackMin, StackSize
    ofstream ofs;
    if(tid==0)
    {
        ofs.open(img_name + ".stackmap.csv");
        ofs << "tid,stackmax,sz"<<endl;
    }
    else
    {
        ofs.open(img_name + ".stackmap.csv", std::ios_base::app);
    }
    ofs << REAL_TID(tid) <<"," << stackmap[pid] <<","<< GetStackSize() << endl;
    ofs.close();

}


//retrieve structures names address and size
int getStructs(const char* file);
string get_struct_name(string str, int ln, string fname, int rec);

string get_complex_struct_name(int ln, string fname)
{
    ifstream fstr(fname);
    int lastmalloc=0;
    // Find the real malloc line
    string line,allocstr;
    for(int i=0; i< ln; ++i)
    {
        getline(fstr, line);
        if(line.find("alloc")!=string::npos)
        {
            allocstr=line;
            lastmalloc=i;
        }
    }
    fstr.close();
    if(allocstr.find("=")==string::npos)
    {
        /*
         * Allocation split among several lines,
         * we assume it looks like
         *  foo =
         *      malloc(bar)
         *  Note:
         *      if foo and '=' are on different lines, we will give up
         */
        fstr.open(fname);
        for(int i=0; i< lastmalloc; ++i)
        {
            getline(fstr, line);
            if(line.find("=")!=string::npos)
                allocstr=line;
        }
        fstr.close();
    }
    //Now that we have the good line, extract the struct name
    return get_struct_name(allocstr, ln, fname, 1/*forbid recursive calls*/);
}

string get_struct_name(string str, int ln, string fname, int hops)
{
    if( str.find(string("alloc"))==string::npos && hops==0)
        return get_complex_struct_name(ln, fname); //Return Ip is not malloc line
    // Remove everything after first '='
    string ret=str.substr(0,str.find('='));
    //remove trailing whitespaces
    while(ret.back()==' ')
        ret.resize(ret.size()-1);
    // Take the last word
    ret=ret.substr(ret.find_last_of(string(" )*"))+1)+string(":")+fname+
        string(":")+str;
    // Our search have failed, it will be an anonymous malloc
    // if(ret.compare("")==0)
    // {
    //     cerr << "Unable to find a suitable alloc name for file  "
    //         << fname << " l: " << ln << endl;
    //     return string("AnonymousStruct");
    // }
    return ret;
}

VOID PREMALLOC(ADDRINT retip, THREADID tid, ADDRINT sz)
{
    int col, ln;
    int id=REAL_TID(tid);
    static int warned=0;
    string fname;
    if( (unsigned int) sz >= REAL_PAGESIZE)
    {

        PIN_LockClient();
        PIN_GetSourceLocation 	(retip, &col, &ln, &fname);
        PIN_UnlockClient();
        //TODO: optimize this to read file only once
        ifstream fstr(fname);
        string line;
        if(!fstr)
        {
            if(!warned || fname.compare("")!=0)
            {
                cerr << "Can't open file '" << fname << "', malloc will be anonymous"<< endl;
                cerr << "Have you compiled your program with '-g' flag ?" <<endl;
                warned=1;
            }
            Allocs[id].sym=fname.compare("")==0?"AnonymousStruct":fname+string(":")+line;
        }
        else
        {
            for(int i=0; i< ln; ++i)
                getline(fstr, line);
            fstr.close();
            Allocs[id].sym=get_struct_name(line, ln, fname, 0/*allow recursive calls*/);
        }
        Allocs[id].sz=sz;
        Allocs[id].ended=0;
    }
}
VOID POSTMALLOC(ADDRINT ret, THREADID tid)
{
    int id=REAL_TID(tid);
    static int anonymousId=0;
    if (Allocs[id].ended==0)
    {
        fstructStream << Allocs[id].sym;
        if(Allocs[id].sym.compare("AnonymousStruct")==0)
            fstructStream << anonymousId++;
        fstructStream <<","<<ret<<","<<Allocs[id].sz<<endl;
        Allocs[id].ended=1;
    }
}

VOID binName(IMG img, VOID *v)
{
    if (IMG_IsMainExecutable(img))
    {
        img_name = basename(IMG_Name(img).c_str());
        char fname[255];
        sprintf(fname, "%s.structs.csv", img_name.c_str());
        fstructStream.open(fname);
        fstructStream << "name,start,sz" << endl;

    }
    getStructs(IMG_Name(img).c_str());
    RTN mallocRtn = RTN_FindByName(img, "malloc");
    if (RTN_Valid(mallocRtn))
    {
        RTN_Open(mallocRtn);
        RTN_InsertCall(mallocRtn, IPOINT_BEFORE, (AFUNPTR)PREMALLOC,
                IARG_RETURN_IP, IARG_THREAD_ID,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 0,  IARG_END);
        RTN_InsertCall(mallocRtn, IPOINT_AFTER, (AFUNPTR)POSTMALLOC,
                IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
        RTN_Close(mallocRtn);
    }
}



VOID Fini(INT32 code, VOID *v)
{
    //print_numa();
    fstructStream.close();
    //delete CPU_NODE;

    //cout << endl << "MAXTHREADS: " << MAXTHREADS << " PAGESIZE: " << PAGESIZE << " INTERVAL: " << INTERVAL << endl << endl;
}




int main(int argc, char *argv[])
{
    PIN_InitSymbols();
    if (PIN_Init(argc,argv)) return 1;

    REAL_PAGESIZE=sysconf(_SC_PAGESIZE);
    PAGESIZE = log2(REAL_PAGESIZE);

    //THREADID t = PIN_SpawnInternalThread(mythread, NULL, 0, NULL);
    //if (t!=1)
    //    cerr << "ERROR internal thread " << t << endl;


    //INS_AddInstrumentFunction(trace_memory_page, 0);




    IMG_AddInstrumentFunction(binName, 0);
    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddFiniFunction(Fini, 0);

    PIN_StartProgram();
}

/*
 * The following function is an adaptation of the libelf-howto.c from:
 * http://em386.blogspot.com
 *
 */

#define ERR -1

int getStructs(const char* file)
{
    Elf *elf;                       /* Our Elf pointer for libelf */
    Elf_Scn *scn=NULL;                   /* Section Descriptor */
    Elf_Data *edata=NULL;                /* Data Descriptor */
    GElf_Sym sym;			/* Symbol */
    GElf_Shdr shdr;                 /* Section Header */




    int fd; 		// File Descriptor
    char *base_ptr;		// ptr to our object in memory
    struct stat elf_stats;	// fstat struct
    cout << "Retrieving data structures from file "<< file << endl;

    if((fd = open(file, O_RDONLY)) == ERR)
    {
        cerr << "couldnt open" << file << endl;
        return ERR;
    }

    if((fstat(fd, &elf_stats)))
    {
        cerr << "could not fstat" << file << endl;
        close(fd);
        return ERR;
    }

    if((base_ptr = (char *) malloc(elf_stats.st_size)) == NULL)
    {
        cerr << "could not malloc" << endl;
        close(fd);
        return ERR;
    }

    if((read(fd, base_ptr, elf_stats.st_size)) < elf_stats.st_size)
    {
        cerr << "could not read" << file << endl;
        free(base_ptr);
        close(fd);
        return ERR;
    }

    /* Check libelf version first */
    if(elf_version(EV_CURRENT) == EV_NONE)
    {
        cerr << "WARNING Elf Library is out of date!" << endl;
    }

    elf = elf_begin(fd, ELF_C_READ, NULL);	// Initialize 'elf' pointer to our file descriptor

    elf = elf_begin(fd, ELF_C_READ, NULL);

    int symbol_count;
    int i;

    while((scn = elf_nextscn(elf, scn)) != NULL)
    {
        gelf_getshdr(scn, &shdr);
        // Get the symbol table
        if(shdr.sh_type == SHT_SYMTAB)
        {
            // edata points to our symbol table
            edata = elf_getdata(scn, edata);
            // how many symbols are there? this number comes from the size of
            // the section divided by the entry size
            symbol_count = shdr.sh_size / shdr.sh_entsize;
            // loop through to grab all symbols
            for(i = 0; i < symbol_count; i++)
            {
                // libelf grabs the symbol data using gelf_getsym()
                gelf_getsym(edata, i, &sym);
                // Keep only objects big enough to be data structures
                if(ELF32_ST_TYPE(sym.st_info)==STT_OBJECT &&
                        sym.st_size >= REAL_PAGESIZE)
                {
                    fstructStream << elf_strptr(elf, shdr.sh_link, sym.st_name) <<
                        "," << sym.st_value << "," << sym.st_size << endl;
                }
            }
        }
    }
    return 0;
}