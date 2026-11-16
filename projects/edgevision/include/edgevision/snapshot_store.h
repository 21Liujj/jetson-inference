#pragma once
#include "edgevision/frame_lease.h"
#include <deque>
#include <filesystem>
#include <functional>

namespace edgevision {
struct SnapshotRecord {
  std::string id, path;
  FrameMeta frame;
  uint64_t task_id=0, saved_mono_ns=0;
  std::size_t encoded_bytes=0;
};
// Owned ephemeral snapshot cache, called only by the physical executor worker.
// The codec is supplied at the application boundary; no ROS/OpenCV dependency.
// Prior evs-*.png cache files are cleared on a new runtime instance. Reports
// requiring durable images must copy them into their own bounded report store.
class SnapshotStore {
 public:
  using Writer=std::function<void(const Frame&,const std::filesystem::path&)>;
  SnapshotStore(std::filesystem::path root,Writer,unsigned capacity=16,std::size_t budget=32*1024*1024);
  SnapshotRecord save(const Frame&,uint64_t task_id,uint64_t now_mono_ns);
  SnapshotRecord query(const std::string&,uint64_t generation,uint64_t now_mono_ns) const;
  FrameLease replay(const std::string&,uint64_t generation,uint64_t now_mono_ns) const;
  const std::filesystem::path& root()const{return root_;}
  std::size_t size()const{return entries_.size();}
  std::size_t retained_bytes()const{return retained_bytes_;}
 private:
  struct Entry{SnapshotRecord record;Frame frame;};
  const Entry& find(const std::string&,uint64_t,uint64_t) const;
  void evict();
  std::filesystem::path root_;
  Writer writer_;
  const unsigned capacity_;
  const std::size_t budget_;
  uint64_t instance_=0;
  std::size_t retained_bytes_=0;
  std::deque<Entry> entries_;
};
}
