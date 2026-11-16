#include "edgevision/preview_cache.h"
#include <cassert>
#include <iostream>
using namespace edgevision;
int main(){
  PreviewCache cache(32,100); Frame source,out;
  assert(!cache.read(out)); source.data={1,2,3,4}; source.bytesused=4;
  source.session_generation=1; source.frame_id=1;source.host_dequeue_mono_ns=100;
  cache.offer(source); source.data[0]=99;
  assert(cache.read(out) && out.frame_id==1 && out.data[0]==1 && out.data.size()==4);
  source.frame_id=2;source.host_dequeue_mono_ns=150;cache.offer(source);
  assert(cache.read(out) && out.frame_id==1);
  source.session_generation=2;cache.offer(source);
  assert(cache.read(out) && out.session_generation==2 && out.frame_id==2);
  source.bytesused=33;cache.offer(source);
  assert(cache.copied()==2 && cache.read(out) && out.bytesused==4);
  std::cout<<"PASS bounded owned preview, throttling, generation, invalid payload\n";
}
