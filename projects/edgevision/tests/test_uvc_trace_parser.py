"""Meaningful correlation contract: sequence reuse must not cross stream lifetimes."""
import importlib.util
import pathlib
import unittest

spec=importlib.util.spec_from_file_location('uvc_parser',pathlib.Path(__file__).resolve().parents[1]/'tools/parse_uvc_trace.py')
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)


class CorrelationTest(unittest.TestCase):
    def test_generation_and_wrap(self):
        frames=[]; rows=[]
        for sid,generation,seq,at in [(11,1,4294967295,1000),(11,1,0,2000),(12,3,0,10000)]:
            frames.append(dict(usb='1-2.1',interface=1,video_minor=0,stream_id=sid,index=0,sequence=seq,bytesused=10,state=4,error=0,reason=0,completed_ns=at))
            rows.append(dict(generation=generation,sequence=seq,host_dq_ns=at+10,publish_ns=at+20,consume_ns=at+30))
        timeline,missing,mapping=module.correlate(frames,rows,100)
        self.assertEqual(len(timeline),3); self.assertFalse(missing)
        self.assertEqual(mapping[3][-1],12)
        self.assertEqual(timeline[1]['complete_to_dq_ns'],10)

    def test_ambiguity_and_missing_do_not_guess(self):
        e=dict(usb='1-2.1',interface=1,video_minor=0,stream_id=1,index=0,sequence=0,bytesused=10,state=4,error=0,reason=0,completed_ns=1000)
        row=dict(generation=1,sequence=0,host_dq_ns=1010,publish_ns=1020,consume_ns=1030)
        a,b,_=module.correlate([e,{**e,'stream_id':2}],[row],100)
        self.assertFalse(a); self.assertEqual(b[0]['candidate_count'],2)
        a,b,_=module.correlate([{**e,'reason':1}],[row],100)
        self.assertFalse(a); self.assertEqual(b[0]['candidate_count'],0)


if __name__=='__main__': unittest.main()
