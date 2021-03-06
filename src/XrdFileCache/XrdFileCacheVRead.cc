#include "XrdFileCacheFile.hh"
#include "XrdFileCache.hh"
#include "XrdFileCacheTrace.hh"

#include "XrdFileCacheInfo.hh"
#include "XrdFileCacheStats.hh"
#include "XrdFileCacheIO.hh"

#include "XrdOss/XrdOss.hh"
#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClFile.hh"
#include "XrdCl/XrdClXRootDResponses.hh"

namespace XrdFileCache
{
// a list of IOVec chuncks that match a given block index
// first element is block index, the following vector elements are chunk readv indicies
struct  ReadVChunkListDisk
{
   ReadVChunkListDisk(int i) : block_idx(i) {}

   int block_idx;
   std::vector<int> arr;
};

struct  ReadVChunkListRAM
{
   ReadVChunkListRAM(Block*b, std::vector <int>* iarr) : block(b), arr(iarr) {}

   Block            *block;
   std::vector<int> *arr;
};

// RAM
struct ReadVBlockListRAM
{
   std::vector<XrdFileCache::ReadVChunkListRAM> bv;

   bool AddEntry(Block* block, int chunkIdx)
   {
      for (std::vector<ReadVChunkListRAM>::iterator i = bv.begin(); i != bv.end(); ++i)
      {
         if (i->block == block)
         {
            i->arr->push_back(chunkIdx);
            return false;
         }
      }
      bv.push_back(ReadVChunkListRAM(block, new std::vector<int>));
      bv.back().arr->push_back(chunkIdx);
      return true;
   }
};

// Disk
struct ReadVBlockListDisk
{
   std::vector<ReadVChunkListDisk> bv;

   void AddEntry(int blockIdx, int chunkIdx)
   {
      for (std::vector<ReadVChunkListDisk>::iterator i = bv.begin(); i != bv.end(); ++i)
      {
         if (i->block_idx == blockIdx)
         {
            i->arr.push_back(chunkIdx);
            return;
         }
      }
      bv.push_back(XrdFileCache::ReadVChunkListDisk(blockIdx));
      bv.back().arr.push_back(chunkIdx);
   }
};
}

using namespace XrdFileCache;

//------------------------------------------------------------------------------

int File::ReadV(const XrdOucIOVec *readV, int n)
{
   if ( ! isOpen())
   {
      return m_io->GetInput()->ReadV(readV, n);
   }

   TRACEF(Dump, "ReadV for " << n << " chunks.");

   if ( ! VReadValidate(readV, n))
   {
      errno = EINVAL;
      return -1;
   }

   Stats loc_stats;

   int bytesRead = 0;

   ReadVBlockListRAM              blocks_to_process;
   std::vector<ReadVChunkListRAM> blks_processed;
   ReadVBlockListDisk             blocks_on_disk;
   std::vector<XrdOucIOVec>       chunkVec;
   DirectResponseHandler         *direct_handler = 0;

   // TODO The following call never fails (other than with out of mem exception).
   // This should be implemented in PrepareBlockRequest().
   if ( ! VReadPreProcess(readV, n, blocks_to_process, blocks_on_disk, chunkVec))
   {
      bytesRead = -1;
      errno = ENOMEM;
   }

   // issue a client read

   if (bytesRead >= 0)
   {
      if ( ! chunkVec.empty())
      {
         direct_handler = new DirectResponseHandler(1);
         m_io->GetInput()->ReadV(*direct_handler, &chunkVec[0], chunkVec.size());
      }
   }

   // disk read
   if (bytesRead >= 0)
   {
      int dr = VReadFromDisk(readV, n, blocks_on_disk);
      if (dr < 0)
      {
         bytesRead = dr;
      }
      else
      {
         bytesRead += dr;
         loc_stats.m_BytesDisk += dr;
      }
   }

   // read from cached blocks
   if (bytesRead >= 0)
   {
      int br = VReadProcessBlocks(readV, n, blocks_to_process.bv, blks_processed);
      if (br < 0)
      {
         bytesRead = br;
      }
      else
      {
         bytesRead += br;
         loc_stats.m_BytesRam += br;
      }
   }

   // check direct requests have arrived, get bytes read from read handle
   if (bytesRead >= 0 && direct_handler != 0)
   {
      XrdSysCondVarHelper _lck(direct_handler->m_cond);

      while (direct_handler->m_to_wait > 0)
      {
         direct_handler->m_cond.Wait();
      }

      if (direct_handler->m_errno == 0)
      {
         for (std::vector<XrdOucIOVec>::iterator i = chunkVec.begin(); i != chunkVec.end(); ++i)
         {
            bytesRead += i->size;
            loc_stats.m_BytesMissed += i->size;
         }
      }
      else
      {
         errno = -direct_handler->m_errno;
         bytesRead = -1;
      }
   }

   {
      XrdSysCondVarHelper _lck(m_downloadCond);

      // decrease ref count on the remaining blocks
      // this happens in case read process has been broke due to previous errors
      for (std::vector<ReadVChunkListRAM>::iterator i = blocks_to_process.bv.begin(); i != blocks_to_process.bv.end(); ++i)
         dec_ref_count(i->block);

      for (std::vector<ReadVChunkListRAM>::iterator i = blks_processed.begin(); i != blks_processed.end(); ++i)
         dec_ref_count(i->block);
   }

   // remove objects on heap
   delete direct_handler;
   for (std::vector<ReadVChunkListRAM>::iterator i = blocks_to_process.bv.begin(); i != blocks_to_process.bv.end(); ++i)
      delete i->arr;
   for (std::vector<ReadVChunkListRAM>::iterator i = blks_processed.begin(); i != blks_processed.end(); ++i)
      delete i->arr;

   m_stats.AddStats(loc_stats);

   TRACEF(Dump, "VRead exit, total = " << bytesRead);
   return bytesRead;
}

//------------------------------------------------------------------------------

bool File::VReadValidate(const XrdOucIOVec *vr, int n)
{
   for (int i = 0; i < n; ++i)
   {
      if (vr[i].offset < 0 || vr[i].offset >= m_fileSize ||
                         vr[i].offset + vr[i].size > m_fileSize)
      {
         return false;
      }
   }
   return true;
}

//------------------------------------------------------------------------------

bool File::VReadPreProcess(const XrdOucIOVec *readV, int n,
                           ReadVBlockListRAM        &blocks_to_process,
                           ReadVBlockListDisk       &blocks_on_disk,
                           std::vector<XrdOucIOVec> &chunkVec)
{
   BlockList_t blks_to_request;

   m_downloadCond.Lock();

   for (int iov_idx = 0; iov_idx < n; iov_idx++)
   {
      const int blck_idx_first =  readV[iov_idx].offset / m_cfi.GetBufferSize();
      const int blck_idx_last  = (readV[iov_idx].offset + readV[iov_idx].size - 1) / m_cfi.GetBufferSize();

      for (int block_idx = blck_idx_first; block_idx <= blck_idx_last; ++block_idx)
      {
         TRACEF(Dump, "VReadPreProcess chunk "<<  readV[iov_idx].size << "@"<< readV[iov_idx].offset);

         BlockMap_i bi = m_block_map.find(block_idx);
         if (bi != m_block_map.end())
         {
            if (blocks_to_process.AddEntry(bi->second, iov_idx))
               inc_ref_count(bi->second);

            TRACEF(Dump, "VReadPreProcess block "<< block_idx <<" in map");
         }
         else if (m_cfi.TestBit(offsetIdx(block_idx)))
         {
            blocks_on_disk.AddEntry(block_idx, iov_idx);

            TRACEF(Dump, "VReadPreProcess block "<< block_idx <<" , chunk idx = " << iov_idx << " on disk");
         }
         else
         {
            if (Cache::GetInstance().RequestRAMBlock())
            {
               Block *b = PrepareBlockRequest(block_idx, false);
               // TODO this can not fail (other than out of memory which we don't handle).
               if (! b) return false;
               inc_ref_count(b);
               blocks_to_process.AddEntry(b, iov_idx);
               blks_to_request.push_back(b);

               TRACEF(Dump, "VReadPreProcess request block " << block_idx);
            }
            else
            {
               long long off;      // offset in user buffer
               long long blk_off;      // offset in block
               long long size;      // size to copy
               const long long BS = m_cfi.GetBufferSize();
               overlap(block_idx, BS, readV[iov_idx].offset, readV[iov_idx].size, off, blk_off, size);
               chunkVec.push_back(XrdOucIOVec2(readV[iov_idx].data+off, BS*block_idx + blk_off,size));

               TRACEF(Dump, "VReadPreProcess direct read " << block_idx);
            }
         }
      }
   }

   m_downloadCond.UnLock();

   ProcessBlockRequests(blks_to_request);

   return true;
}

//------------------------------------------------------------------------------

int File::VReadFromDisk(const XrdOucIOVec *readV, int n, ReadVBlockListDisk& blocks_on_disk)
{
   int bytes_read = 0;
   for (std::vector<ReadVChunkListDisk>::iterator bit = blocks_on_disk.bv.begin(); bit != blocks_on_disk.bv.end(); ++bit )
   {
      int blockIdx = bit->block_idx;
      for (std::vector<int>::iterator chunkIt = bit->arr.begin(); chunkIt != bit->arr.end(); ++chunkIt)
      {
         int chunkIdx = *chunkIt;

         long long off;     // offset in user buffer
         long long blk_off;    // offset in block
         long long size;    // size to copy

         TRACEF(Dump, "VReadFromDisk block= " << blockIdx <<" chunk=" << chunkIdx);

         overlap(blockIdx, m_cfi.GetBufferSize(), readV[chunkIdx].offset, readV[chunkIdx].size, off, blk_off, size);

         int rs = m_output->Read(readV[chunkIdx].data + off,  blockIdx*m_cfi.GetBufferSize() + blk_off - m_offset, size);
         if (rs >= 0)
         {
            bytes_read += rs;
         }
         else
         {
            // ofs read should set the errno
            TRACEF(Error, "VReadFromDisk FAILED block=" << blockIdx << " chunk=" << chunkIdx << " off= " << off  << " blk_off=" <<  blk_off << " size = " << size <<  "chunOff " << readV[chunkIdx].offset);

            return -1;
         }
      }
   }

   return bytes_read;
}

//------------------------------------------------------------------------------

int File::VReadProcessBlocks(const XrdOucIOVec *readV, int n,
                             std::vector<ReadVChunkListRAM>& blocks_to_process,
                             std::vector<ReadVChunkListRAM>& blocks_processed)
{
   int bytes_read = 0;
   while ((! blocks_to_process.empty()) && (bytes_read >= 0))
   {
      std::vector<ReadVChunkListRAM> finished;
      {
         XrdSysCondVarHelper _lck(m_downloadCond);
         std::vector<ReadVChunkListRAM>::iterator bi = blocks_to_process.begin();
         while (bi != blocks_to_process.end())
         {
            if (bi->block->is_finished())
            {
               finished.push_back(ReadVChunkListRAM(bi->block, bi->arr));
               // Here we rely on the fact that std::vector does not reallocate on erase!
               blocks_to_process.erase(bi);
            }
            else
            {
               ++bi;
            }
         }

         if (finished.empty())
         {
            m_downloadCond.Wait();
            continue;
         }
      }

      std::vector<ReadVChunkListRAM>::iterator bi = finished.begin();
      while (bi != finished.end())
      {
         if (bi->block->is_ok())
         {
            for (std::vector<int>::iterator chunkIt = bi->arr->begin(); chunkIt < bi->arr->end(); ++chunkIt)
            {
               long long off;      // offset in user buffer
               long long blk_off;      // offset in block
               long long size;      // size to copy

               int block_idx = bi->block->m_offset/m_cfi.GetBufferSize();
               overlap(block_idx, m_cfi.GetBufferSize(), readV[*chunkIt].offset, readV[*chunkIt].size, off, blk_off, size);
               memcpy(readV[*chunkIt].data + off,  &(bi->block->m_buff[blk_off]), size);
               bytes_read += size;
            }
         }
         else
         {
            bytes_read = -1;
            errno = -bi->block->m_errno;
            break;
         }

         ++bi;
      }

      // add finished to processed list
      std::copy(finished.begin(), finished.end(), std::back_inserter(blocks_processed));
      finished.clear();
   }

   TRACEF(Dump, "VReadProcessBlocks total read  " <<  bytes_read);

   return bytes_read;
}
