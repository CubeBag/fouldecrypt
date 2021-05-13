#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <mach-o/loader.h>
#include <time.h>
#include <sys/time.h>

#include <mach/mach.h>

#include "kerninfra/kerninfra.hpp"

#undef PAGE_SIZE
#define PAGE_SIZE 0x4000
#define EXEC_PAGE_SIZE 0x1000

int VERBOSE = 0;
#define DLOG(f_, ...)                                                                            \
{                                                                                                 \
    if (VERBOSE) {                                                                                \
        struct tm _tm123_;                                                                            \
        struct timeval _xxtv123_;                                                                     \
        gettimeofday(&_xxtv123_, NULL);                                                               \
        localtime_r(&_xxtv123_.tv_sec, &_tm123_);                                                     \
        printf("%02d:%02d:%02d.%06d\t", _tm123_.tm_hour, _tm123_.tm_min, _tm123_.tm_sec, _xxtv123_.tv_usec); \
        printf((f_), ##__VA_ARGS__);                                                                  \
        printf("\n");                                                                               \
    }                                                                                              \
};


extern "C" int mremap_encrypted(void*, size_t, uint32_t, uint32_t, uint32_t);
/*
static int
unprotect(int f, uint8_t *dupe, struct encryption_info_command_64 *info)
{
    vm_address_t offalign = info->cryptoff & ~(PAGE_SIZE - 1);
    vm_address_t mapoffset = info->cryptoff - offalign;
    size_t aligned_size = info->cryptsize + mapoffset;
    size_t realsize = info->cryptsize;
    int cryptid = info->cryptid;
    DLOG("mapping encrypted data pages using off: 0x%lx, size: 0x%zx", offalign, aligned_size);
    
    void *tmp_dec_area = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, 0, 0);
    
    
    auto curp = proc_t_p(current_proc());
    auto vPageShift = curp.task()._map().page_shift();
    DLOG("original page shift: %d", vPageShift.load());
    vPageShift.store(12);
    DLOG("new page shift: %d", vPageShift.load());

    //int fpid = fork();
    int fpid = 0;
    if (fpid < 0) {
        perror("fork(unprotect)");
    } else if (fpid == 0) {
        exit(0);
        // we are child!
        void *oribase = mmap(NULL, aligned_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, f, offalign);
        if (oribase == MAP_FAILED) {
            perror("mmap(unprotect)");
            return 1;
        }

        void *base = (char *)oribase + mapoffset;
        DLOG("mremap_encrypted pages using addr: %p, size: 0x%lx, cryptid: %d, cputype: %x, cpusubtype: %x", 
            base, aligned_size, cryptid, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL);
        //int error = mremap_encrypted(oribase, aligned_size, info->cryptid, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL);
        int error = mremap_encrypted(base, realsize, cryptid, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL);
        if (error) {
            perror("mremap_encrypted(unprotect)");
            munmap(oribase, info->cryptsize);
            return 1;
        }

        memmove(tmp_dec_area, base, realsize);

        DLOG("cleaning up...");
        munmap(oribase, info->cryptsize);

        exit(0);
    } else {
        vPageShift.store(14);
        DLOG("restored page shift: %d", vPageShift.load());
        wait(NULL);
    }
    
    DLOG("copying child process's ret pages..");
    //memcpy(dupe + info->cryptoff, base + (info->cryptoff - offalign), info->cryptsize);
    memcpy(dupe + info->cryptoff, tmp_dec_area, info->cryptsize);

    return 0;
}*/

extern "C" kern_return_t mach_vm_remap(vm_map_t, mach_vm_address_t *, mach_vm_size_t,
                            mach_vm_offset_t, int, vm_map_t, mach_vm_address_t,
                            boolean_t, vm_prot_t *, vm_prot_t *, vm_inherit_t);

void *__mmap(const char *info, void *base, size_t size, int prot, int flags, int fd, size_t off) {
    DLOG("-->> %s mmaping(%p, 0x%zx, %d, 0x%x, %d, 0x%zx)", info, base, size, prot, flags, fd, off);
    void *ret = mmap(base, size, prot, flags, fd, off);
    if (ret == MAP_FAILED) {
        perror("mmap");
    }
    DLOG("<<-- %s mmaping(%p, 0x%zx, %d, 0x%x, %d, 0x%zx) = %p", info, base, size, prot, flags, fd, off, ret);
    return ret;
}

int __mremap_encrypted(const char *info, void *base, size_t cryptsize, uint32_t cryptid, uint32_t cpuType, uint32_t cpuSubType) {
    DLOG("<<-- %s mremap_encrypted(%p, 0x%zx, %d, 0x%x, 0x%x)", info, base, cryptsize, cryptid, cpuType, cpuSubType);
    int ret = mremap_encrypted(base, cryptsize, cryptid, cpuType, cpuSubType);
    if (ret) {
        perror("mremap_encrypted");
    }
    DLOG("-->> %s mremap_encrypted(%p, 0x%zx, %d, 0x%x, 0x%x) = %d", info, base, cryptsize, cryptid, cpuType, cpuSubType, ret);
    return ret;
}

static int
unprotect(int f, uint8_t *dupe, struct encryption_info_command_64 *info)
{
    assert((info->cryptoff & (EXEC_PAGE_SIZE - 1)) == 0);

    DLOG("Going to decrypt crypt page: off 0x%x size 0x%x cryptid %d", info->cryptoff, info->cryptsize, info->cryptid);

    int cryptoff = info->cryptoff;
    int cryptsize = info->cryptsize;

    size_t off_aligned = info->cryptoff & ~(PAGE_SIZE - 1);
    size_t size_aligned = (info->cryptsize & ~(PAGE_SIZE - 1)) + PAGE_SIZE;
    //size_t size_aligned = info->cryptsize + info->cryptoff - off_aligned;
    size_t map_offset = info->cryptoff - off_aligned;
    
    void *tmp_map = __mmap("tmp_map cache", NULL, size_aligned, PROT_READ | PROT_EXEC, MAP_PRIVATE, f, off_aligned);
    
    void *tmp_read = malloc(size_aligned);
    memcpy(tmp_read, tmp_map, size_aligned);
    
    void *tmpshare = __mmap("share region for fork", NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, 0, 0);

    void *oribase = NULL;
    if (!(info->cryptoff & (PAGE_SIZE - 1))) {
        // already 16k aligned, pretty good!
        DLOG("Already 16k aligned, directly go ahead :)");
        oribase = __mmap("16k-aligned", NULL, size_aligned, PROT_READ | PROT_EXEC, MAP_PRIVATE, f, info->cryptoff);
    } else {
        DLOG("Not 16k aligned, trying to do the hack :O");
        int fpid = fork();
        
        if (fpid == 0) {
            if (!!init_kerninfra()) {
                fprintf(stderr, "Failed to init kerninfra!!\n");
                exit(1);
            } else {
                DLOG("successfully initialized kerninfra!");
            }

            /*oribase = mmap(NULL, size_aligned, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
            if (oribase == MAP_FAILED) {
                perror("mmap_pre(unprotect)");
                return 1;
            }*/

            // now map the 4K-aligned enc pages, like the good old days
            DLOG("mapping encrypted data pages using off: 0x%x, size: 0x%x", cryptoff, cryptsize);
            //oribase = mmap(NULL, 0x900, PROT_READ | PROT_EXEC, MAP_PRIVATE, f, 0x5000);
            //oribase = mmap(oribase, size_aligned + off_aligned - map_offset, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, f, map_offset);
            //oribase = __mmap("4k-aligned mmap", NULL, info->cryptsize, PROT_READ | PROT_EXEC, MAP_PRIVATE, f, info->cryptoff);
            
            // DEBUG test no offset
            oribase = __mmap("4k-aligned mmap", NULL, size_aligned, PROT_READ | PROT_EXEC, MAP_PRIVATE, f, off_aligned);

            *(void **)tmpshare = oribase;

            // patching kernel task map to allow 4K page (MAGIC)
            auto curp = proc_t_p(current_proc());
            auto vPageShift = curp.task()._map().page_shift();
            DLOG("original page shift: %d", vPageShift.load());
            vPageShift.store(12);
            DLOG("new page shift: %d", vPageShift.load());


            while (true) {
                usleep(100000);
            }
            
            // restore kernel task map to 16K page, or it will panic because encrypting compressor's paging
            vPageShift.store(14);
            DLOG("restored page shift: %d", vPageShift.load());
            
            if (oribase == MAP_FAILED) {
                //perror("mmap(unprotect)");
                return 1;
            }
            exit(0);
        } else {
            usleep(1000000);

            kern_return_t kr;
            mach_port_t port = MACH_PORT_NULL;
            kr = task_for_pid(mach_task_self(), fpid, &port);
            if (kr != KERN_SUCCESS) {
                DLOG("error tfp fork child! kr: %d", kr);
                exit(1);
            }
            void *tmptarget = __mmap("remap target", NULL, cryptsize, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
            uint64_t child_addr = *(uint64_t *)tmpshare;
            child_addr += map_offset;
            mach_vm_address_t dest_page_address_ = (mach_vm_address_t)tmptarget;
            vm_prot_t         curr_protection, max_protection;
            DLOG("remapping from child's %p to our's %p", (void *)child_addr, tmptarget);
            kr = mach_vm_remap(mach_task_self(), &dest_page_address_, cryptsize, 0, VM_FLAGS_OVERWRITE | VM_FLAGS_FIXED, port,
                                    (mach_vm_address_t)child_addr, TRUE, &curr_protection, &max_protection, VM_INHERIT_COPY);
            if (kr != KERN_SUCCESS) {
                DLOG("error remapping page! kr: %d", kr);
                exit(1);
            }
            
            oribase = tmptarget;
        }

        

        //exit(111);
        /*if (kr != KERN_SUCCESS) {
            DLOG("error remapping page! kr: %d", kr);
            exit(1);
        }*/
        
    }

    /*int ret;
    ret = mprotect(oribase, size_aligned, PROT_READ | PROT_WRITE);
    if (ret) {
        perror("mprotect");
        return 1;
    }
    

    ret = mprotect(oribase, size_aligned, PROT_READ | PROT_EXEC);
    if (ret) {
        perror("mprotect");
        return 1;
    }*/
        
    
    // old-school mremap_encrypted
    void *base = (char *)oribase;
    DLOG("mremap_encrypted pages using addr: %p, size: 0x%x, cryptid: %d, cputype: %x, cpusubtype: %x", 
        base, info->cryptsize, info->cryptid, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL);
    int error = __mremap_encrypted("unprotect", base, info->cryptsize, info->cryptid, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL);
    if (error) {
        //perror("mremap_encrypted(unprotect)");
        munmap(oribase, info->cryptsize);
        return 1;
    }

    DLOG("copying enc pages, size: 0x%x..", info->cryptsize);
    memcpy(dupe + info->cryptoff, base, info->cryptsize);

    DLOG("cleaning up...");
    munmap(oribase, info->cryptsize);

    return 0;
}

static uint8_t*
map(const char *path, bool _mutable, size_t *size, int *descriptor)
{
    int f = open(path, _mutable ? O_CREAT | O_TRUNC | O_RDWR : O_RDONLY, 0755);
    if (f < 0) {
        perror(_mutable ? "open(map-ro)" : "open(map-rw)");
        return NULL;
    }
    
    if (_mutable) {
        if (ftruncate(f, *size) < 0) {
            perror("ftruncate(map)");
            return NULL;
        }
    }

    struct stat s;
    if (fstat(f, &s) < 0) {
        perror("fstat(map)");
        close(f);
        return NULL;
    }

    uint8_t *base = (uint8_t *)mmap(NULL, s.st_size, _mutable ? PROT_READ | PROT_WRITE : PROT_READ,
        _mutable ? MAP_SHARED : MAP_PRIVATE, f, 0);
    if (base == MAP_FAILED) {
        perror(_mutable ? "mmap(map-ro)" : "mmap(map-rw)");
        close(f);
        return NULL;
    }

    *size = s.st_size;
    if (descriptor) {
        *descriptor = f;
    } else {
        close(f);
    }
    return base;
}

int
decrypt_macho(const char *inputFile, const char *outputFile)
{
    DLOG("mapping input file: %s", inputFile);
    size_t base_size;
    int f;
    uint8_t *base = map(inputFile, false, &base_size, &f);
    if (base == NULL) {
        return 1;
    }
    
    DLOG("mapping output file: %s", outputFile);
    size_t dupe_size = base_size;
    uint8_t *dupe = map(outputFile, true, &dupe_size, NULL);
    if (dupe == NULL) {
        munmap(base, base_size);
        return 1;
    }

    // If the files are not of the same size, then they are not duplicates of
    // each other, which is an error.
    //
    if (base_size != dupe_size) {
        munmap(base, base_size);
        munmap(dupe, dupe_size);
        return 1;
    }

    DLOG("finding encryption_info segment in file...");
    struct mach_header_64* header = (struct mach_header_64*) base;
    assert(header->magic == MH_MAGIC_64);
    assert(header->cputype == CPU_TYPE_ARM64);
    assert(header->cpusubtype == CPU_SUBTYPE_ARM64_ALL);

    uint32_t offset = sizeof(struct mach_header_64);

    // Enumerate all load commands and check for the encryption header, if found
    // start "unprotect"'ing the contents.
    //
    struct encryption_info_command_64 *encryption_info = NULL;
    for (uint32_t i = 0; i < header->ncmds; i++) {
        struct load_command* command = (struct load_command*) (base + offset);

        if (command->cmd == LC_ENCRYPTION_INFO_64) {
            DLOG("    found encryption_info segment at offset %x", offset);
            encryption_info = (struct encryption_info_command_64*) command;
            // There should only be ONE header present anyways, so stop after
            // the first one.
            //
            break;
        }

        offset += command->cmdsize;
    }
    if (!encryption_info || !encryption_info->cryptid) {
        fprintf(stderr, "file not encrypted!\n");
        exit(1);
    }
    // If "unprotect"'ing is successful, then change the "cryptid" so that
    // the loader does not attempt to decrypt decrypted pages.
    //
    DLOG("copying original data of size 0x%zx...", base_size);
    memcpy(dupe, base, base_size);
    
    DLOG("decrypting encrypted data...");
    if (unprotect(f, dupe, encryption_info) == 0) {
        encryption_info = (struct encryption_info_command_64*) (dupe + offset);
        encryption_info->cryptid = 0;
    }

    munmap(base, base_size);
    munmap(dupe, dupe_size);
    return 0;
}

int
main(int argc, char* argv[])
{
    int opt;
    while((opt = getopt(argc, argv, "v")) != -1) {
        switch (opt) {
            case 'v':
                VERBOSE = 1;
                break;
            default:
                printf("optopt = %c\n", (char)optopt);
                printf("opterr = %d\n", opterr);
                fprintf(stderr, "usage: %s [-v] encfile outfile\n", argv[0]);
                exit(1);
        } 
    }
    argc -= optind;
    argv += optind;
    if (argc < 2) {
        fprintf(stderr, "usage: fouldecrypt [-v] encfile outfile\n");
        return 1;
    }
    return decrypt_macho(argv[0], argv[1]);
}
