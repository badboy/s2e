#include <iostream>
#include <exception>
#include <malloc.h>

//#define DEBUG_ALLOC
//#define TESTSUITE_ALLOC

#include "Slab.h"

#ifdef _WIN32
#include <windows.h>
#else

#endif

namespace s2e
{

PageAllocator::PageAllocator()
{

}

PageAllocator::~PageAllocator()
{
    RegionMap::iterator it;

    for (it = m_regions.begin(); it != m_regions.end(); ++it) {
        osFree((*it).first);
    }

    RegionSet::iterator sit;
    for (sit = m_busyRegions.begin(); sit != m_busyRegions.end(); ++sit) {
        osFree((*sit));
    }

}

uintptr_t PageAllocator::osAlloc()
{
#ifdef _WIN32
    return(uintptr_t) VirtualAlloc(NULL, getRegionSize(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
#error Not implemented
#endif
}

void PageAllocator::osFree(uintptr_t region)
{
#ifdef _WIN32
    BOOL b = VirtualFree((PVOID)region, 0, MEM_RELEASE);
    assert(b);
#else
#error Not implemented
#endif
}

uintptr_t PageAllocator::allocPage()
{
    RegionMap::iterator it = m_regions.begin();
    if (it == m_regions.end()) {
        uintptr_t region = osAlloc();
        if (!region) {
            return 0;
        }

#ifdef DEBUG_ALLOC
        std::cout << "Allocating new region " << std::hex << region << std::endl;
#endif

        m_regions[region] = ((uint64_t)-1) & ~1LL;
        return region;
    }

    uint64_t index;
    int b = bit_scan_forward_64(&index, (*it).second);
    assert(b);
    (*it).second &= ~(1LL << index);

    uintptr_t reg = (*it).first;

    if ((*it).second == 0) {

#ifdef DEBUG_ALLOC
        std::cout << "Region " << std::hex << reg << " is full" << std::endl;
#endif

        m_busyRegions.insert(reg);
        m_regions.erase(reg);
    }

    return reg + index * getPageSize();
}

void PageAllocator::freePage(uintptr_t page)
{

    RegionMap::iterator it = m_regions.find(page);
    if (it == m_regions.end()) {
#ifdef DEBUG_ALLOC
        std::cout << "busy size " << std::dec << m_busyRegions.size() << std::endl;
        std::cout << "freeing " << std::hex << page << std::endl;
#endif

        RegionSet::iterator itr = m_busyRegions.find(page);
        assert(itr != m_busyRegions.end());
        uint64_t index = (page - (*itr)) / getPageSize();

        m_busyRegions.erase(*itr);
        m_regions[*itr] = (1LL << index);
        return;
    }

    uint64_t index = (page - (*it).first) / getPageSize();

    assert(!((*it).second & (1LL << index)));

    (*it).second |= 1LL << index;
    if ((*it).second == (uint64_t)-1) {
#ifdef DEBUG_ALLOC
        std::cout << "Freeing empty region " << std::hex << (*it).first << std::endl;
#endif

        osFree((*it).first);
        m_regions.erase((*it).first);
    }

    return;
}


BlockAllocator::BlockAllocator(PageAllocator *pa, unsigned blockSizePo2, uint8_t magic)
{
    list_init_head(&m_totallyFreeList);
    list_init_head(&m_freeList);
    list_init_head(&m_busyList);

    m_magic = magic;

    m_freePagesCount = 0;
    m_busyPagesCount = 0;
    m_freeBlocksCount = 0;

    m_pa = pa;

    m_pageSize = pa->getPageSize();
    m_blockSize = 1 << blockSizePo2;
    assert(m_blockSize < 2048);

    m_blocksPerPage = (m_pageSize - sizeof(BlockAllocatorHdr)) / m_blockSize;

    assert((m_blocksPerPage) < sizeof(((BlockAllocatorHdr*)(0))->mask) * 8);
}

uintptr_t BlockAllocator::expand()
{
    uintptr_t newPage = m_pa->allocPage();
    if( !newPage) {
        return 0;
    }

    BlockAllocatorHdr *hdr = (BlockAllocatorHdr*)newPage;
    hdr->init(hdr, m_blocksPerPage);
    hdr->signature = BLOCK_HDR_SIGNATURE | m_magic;

    list_insert_tail(&m_totallyFreeList, &hdr->link);

    m_freePagesCount++;
    m_freeBlocksCount += m_blocksPerPage;
    return newPage;
}

void BlockAllocator::shrink()
{
    list_t *entry;
    BlockAllocatorHdr *page;

    if (list_empty(&m_totallyFreeList)) {
        return;
    }

    entry = list_remove_tail(&m_totallyFreeList);
    page = containing_record(entry, BlockAllocatorHdr, link);
    m_pa->freePage((uintptr_t)page);
    m_freePagesCount--;
    m_freeBlocksCount -= m_blocksPerPage;
}

uintptr_t BlockAllocator::alloc()
{

    if (!m_freeBlocksCount) {
        uintptr_t page = expand();
        if (!page) {
            return 0;
        }
    }

    list_t *freeItem;

    if (list_empty(&m_freeList)) {
        if (list_empty(&m_totallyFreeList)) {
            assert(false);
        }
        freeItem = m_totallyFreeList.next;
    }else {
        freeItem = m_freeList.next;
    }

    BlockAllocatorHdr *page = containing_record(freeItem, BlockAllocatorHdr, link);
    assert((((uintptr_t)page) & (m_pageSize-1)) == 0);

    unsigned fb = page->findFree(m_blocksPerPage);

    page->alloc(fb);
    m_freeBlocksCount--;

    if (page->freeCount == m_blocksPerPage - 1) {
        list_remove_entry(&page->link);
        list_insert_head(&m_freeList, &page->link);
    }

    if (!page->freeCount) {
        list_remove_entry(&page->link);
        list_insert_head(&m_busyList, &page->link);
        m_freePagesCount--;
        m_busyPagesCount++;
    }

    uintptr_t ret = ((uintptr_t)page) + sizeof(BlockAllocatorHdr) + fb * m_blockSize;
    memset((void*)ret, 0xEB, m_blockSize);
    return ret;
}


void BlockAllocator::free(uintptr_t b)
{
    uintptr_t bhdr = b & ~(m_pageSize-1);
    BlockAllocatorHdr *hdr = (BlockAllocatorHdr*)bhdr;

    assert(hdr->signature == (BLOCK_HDR_SIGNATURE | m_magic));


    memset((void*)b, 0xDB, m_blockSize);

    unsigned index = ((b & (m_pageSize-1)) - sizeof(BlockAllocatorHdr)) / m_blockSize;

    hdr->free(index);
    m_freeBlocksCount++;

    if (hdr->freeCount == 1) {
        list_remove_entry(&hdr->link);
        list_insert_head(&m_freeList, &hdr->link);
        m_freePagesCount++;
        m_busyPagesCount--;
      }

      if (hdr->freeCount == m_blocksPerPage) {
        list_remove_entry(&hdr->link);
        list_insert_head(&m_totallyFreeList, &hdr->link);
      }
}

SlabAllocator::SlabAllocator(unsigned minPo2, unsigned maxPo2)
{
    assert(minPo2 <= maxPo2);

    m_minPo2 = minPo2;
    m_maxPo2 = maxPo2;

    m_pa = new PageAllocator();

    m_bas = new BlockAllocator*[m_maxPo2 - m_minPo2];

    for (unsigned i=0; i<=(m_maxPo2 - m_minPo2); ++i) {
        m_bas[i] = new BlockAllocator(m_pa, i + m_minPo2, i + m_minPo2);
    }
}

SlabAllocator::~SlabAllocator()
{
    delete [] m_bas;
    delete m_pa;
}

BlockAllocator *SlabAllocator::getSlab(uintptr_t addr) const
{
    //read the magic number
    uintptr_t bhdr = addr & ~(m_pa->getPageSize()-1);
    const BlockAllocatorHdr *hdr = (const BlockAllocatorHdr*)bhdr;

    if ((hdr->signature & 0xFFFFFF00) != BLOCK_HDR_SIGNATURE) {
        return NULL;
    }

    uint8_t m = hdr->signature & 0xFF;
    if (!(m <= m_maxPo2 && m >= m_minPo2)) {
        std::cerr << std::hex << "invalid signature " << hdr->signature << std::endl;
        assert(false);
    }

    return m_bas[m - m_minPo2];
}

unsigned SlabAllocator::log(size_t size) const
{
    if (size  >= 4 && size <= 8) {
        return 3;
    }else if (size  >= 9 && size <= 16) {
        return 4;
    }else if (size  >= 17 && size <= 32) {
        return 5;
    }else if (size  >= 33 && size <= 64) {
        return 6;
    }else if (size  >= 65 && size <= 128) {
        return 7;
    }else if (size  >= 129 && size <= 256) {
        return 8;
    }

    return 0;
}

uintptr_t SlabAllocator::alloc(size_t size)
{
    unsigned i = log(size);
    if (!i || i < m_minPo2 || i > m_maxPo2) {
        return 0;
    }

    return m_bas[i - m_minPo2]->alloc();
}

bool SlabAllocator::free(uintptr_t addr)
{
    BlockAllocator *b = getSlab(addr);
    if (!b) {
        //std::cerr << "Invalid addr " << std::hex << addr <<std::endl;
        return false;
    }
    b->free(addr);
    return true;
}

bool SlabAllocator::isValid(uintptr_t addr) const
{
    return getSlab(addr) != NULL;
}

static SlabAllocator *s_slab = NULL;

void slab_init()
{
    if (s_slab) {
        return;
    }

    s_slab = new SlabAllocator(3, 8);
}

}


static bool s_inalloc = false;

void* operator new (size_t size)
{
    if (!s2e::s_slab || s_inalloc) {
        void *p = malloc(size);
        if (!p) {
            throw new std::bad_alloc();
        }
        return p;
    }

    s_inalloc = true;
    uintptr_t pr = s2e::s_slab->alloc(size);
    if (pr) {
        s_inalloc = false;
        if (!pr) {
            throw new std::bad_alloc();
        }
        return (void*)pr;
    }

    void *p = malloc(size);
    if (!p) {
        throw new std::bad_alloc();
    }

    s_inalloc = false;

    return p;
}

void operator delete (void *p)
{
    if (!s2e::s_slab) {
        free(p);
        return;
    }

    s_inalloc = true;

    if (!s2e::s_slab->free((uintptr_t)p)) {
        free(p);
    }

    s_inalloc = false;
}




#ifdef TESTSUITE_ALLOC
using namespace s2e;

void testPageAllocator()
{
    std::set<uintptr_t> allocated;

    PageAllocator a;

    unsigned count=0;
    uintptr_t p = 0;
    do {
        p = a.allocPage();
        //std::cout << "Page 0x" << std::hex << p << std::endl;
        ++count;
        if (p) {
            allocated.insert(p);
        }
    }while(p);

    std::cout << "Allocated " << std::dec << count << " pages" << std::endl;

    while(allocated.size() > 0) {
        a.freePage(*allocated.begin());
        allocated.erase(*allocated.begin());
    }
}

void testBlockAllocator(unsigned blockSizePo2)
{
    std::set<uintptr_t> allocated;

    PageAllocator pa;
    BlockAllocator ba(&pa, blockSizePo2);


    unsigned count=0;
    uintptr_t p = 0;

    do {
        p = ba.alloc();
        //std::cout << "Block 0x" << std::hex << p << std::endl;
        ++count;


        assert(allocated.find(p) == allocated.end());
        if (p) {
            try {
            allocated.insert(p);
        }catch(std::exception &e) {
            break;
        }
        }
    }while(p);

    while(allocated.size() > 0) {
        ba.free(*allocated.begin());
        allocated.erase(*allocated.begin());
    }

    std::cout << "Allocated " << std::dec << count << " blocks" << std::endl;

}

void testRandom(unsigned blockSizePo2)
{

}

void testMalloc(unsigned blockSizePo2)
{
    while(malloc(1<<blockSizePo2))
        ;
}

void test()
{
    uint8_t *v1 = new uint8_t;
    uint64_t *v2 = new uint64_t;

    std::cout << "Allocated v1=" << std::hex << (uintptr_t)v1 << std::endl;
    std::cout << "Allocated v2=" << std::hex << (uintptr_t)v2 << std::endl;
}

int main(int argc, char **argv)
{

    test();

#if 0
    //testPageAllocator();
    for (unsigned i=3; i<11; ++i) {
        if (argc > 2) {
            testMalloc(i);
        }else {
            testBlockAllocator(i);
        }
    }
#endif
}
#endif