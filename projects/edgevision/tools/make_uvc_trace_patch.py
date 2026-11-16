#!/usr/bin/env python3
"""Generate the small reproducible GPL UVC patch against the frozen pristine tree."""
import argparse
import difflib
import hashlib
import json
import pathlib


def once(text, old, new):
    if text.count(old)!=1: raise ValueError('SOURCE_CONTEXT_CHANGED: '+old[:80])
    return text.replace(old,new,1)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pristine',type=pathlib.Path,required=True)
    parser.add_argument('--manifest',type=pathlib.Path,required=True)
    parser.add_argument('--out',type=pathlib.Path,required=True)
    args=parser.parse_args()
    manifest=json.loads(args.manifest.read_text(encoding='utf-8')); old={}
    for item in manifest['files']:
        p=args.pristine/item['name']; data=p.read_bytes()
        if hashlib.sha256(data).hexdigest()!=item['sha256']: raise ValueError('PRISTINE_HASH_MISMATCH')
        old[item['name']]=data.decode()
    new=dict(old)
    new['Makefile']+='\n# Trace header re-inclusion for this external-module build.\nccflags-y += -I$(src)\n'
    new['uvcvideo.h']=once(new['uvcvideo.h'],'struct uvc_video_queue {\n\tstruct vb2_queue queue;',
       'struct uvc_video_queue {\n\t/* Assigned before URB submission; stable until all work is drained. */\n\tu64 edgevision_stream_id;\n\tstruct vb2_queue queue;')
    q=new['uvc_queue.c']
    q=once(q,'#include "uvcvideo.h"\n','#include "uvcvideo.h"\n\n#define CREATE_TRACE_POINTS\n#include "uvc_trace.h"\n\n/* Unique per STREAMON within this module load; not userspace generation. */\nstatic atomic64_t edgevision_stream_ids = ATOMIC64_INIT(0);\n\n/* reason: 0=delivered to vb2, 1=internal corrupt-frame recycle, 2=cancel return. */\nstatic void edgevision_trace_buffer(struct uvc_video_queue *queue,\n\t\t\t\t  struct uvc_buffer *buf, u32 reason)\n{\n\tif (queue->queue.type == V4L2_BUF_TYPE_VIDEO_CAPTURE &&\n\t    trace_uvc_frame_enabled()) {\n\t\tstruct uvc_streaming *stream = uvc_queue_to_stream(queue);\n\n\t\ttrace_uvc_frame(stream, buf, READ_ONCE(queue->edgevision_stream_id),\n\t\t\t\treason, ktime_get_ns());\n\t}\n}\n\n/* action: 1=start attempt, 2=start failure, 3=stop, 4=disconnect cancel. */\nstatic void edgevision_trace_stream(struct uvc_video_queue *queue,\n\t\t\t\t  u32 action, int result)\n{\n\tif (queue->queue.type == V4L2_BUF_TYPE_VIDEO_CAPTURE &&\n\t    trace_uvc_stream_enabled())\n\t\ttrace_uvc_stream(uvc_queue_to_stream(queue),\n\t\t\tREAD_ONCE(queue->edgevision_stream_id), action, result,\n\t\t\tktime_get_ns());\n}\n')
    q=once(q,'\t\tbuf->state = state;\n\t\tvb2_buffer_done', '\t\tbuf->state = state;\n\t\tedgevision_trace_buffer(queue, buf, 2);\n\t\tvb2_buffer_done')
    q=once(q,'\tqueue->buf_used = 0;\n\n\tret = uvc_video_start_streaming(stream);',
       '\tqueue->buf_used = 0;\n\tif (vq->type == V4L2_BUF_TYPE_VIDEO_CAPTURE)\n\t\tWRITE_ONCE(queue->edgevision_stream_id,\n\t\t\t   atomic64_inc_return(&edgevision_stream_ids));\n\tedgevision_trace_stream(queue, 1, 0);\n\n\tret = uvc_video_start_streaming(stream);')
    q=once(q,'\tif (ret == 0)\n\t\treturn 0;\n\n\tspin_lock_irq(&queue->irqlock);',
       '\tif (ret == 0)\n\t\treturn 0;\n\tedgevision_trace_stream(queue, 2, ret);\n\n\tspin_lock_irq(&queue->irqlock);')
    q=once(q,'\tlockdep_assert_irqs_enabled();\n\n\tif (vq->type != V4L2_BUF_TYPE_META_CAPTURE)',
       '\tlockdep_assert_irqs_enabled();\n\tedgevision_trace_stream(queue, 3, 0);\n\n\tif (vq->type != V4L2_BUF_TYPE_META_CAPTURE)')
    q=once(q,'\tqueue->queue.type = type;','\tWRITE_ONCE(queue->edgevision_stream_id, 0);\n\tqueue->queue.type = type;')
    q=once(q,'\tspin_lock_irqsave(&queue->irqlock, flags);\n\tuvc_queue_return_buffers(queue, UVC_BUF_STATE_ERROR);',
       '\tspin_lock_irqsave(&queue->irqlock, flags);\n\tif (disconnect)\n\t\tedgevision_trace_stream(queue, 4, -ENODEV);\n\tuvc_queue_return_buffers(queue, UVC_BUF_STATE_ERROR);')
    q=once(q,'\tif ((queue->flags & UVC_QUEUE_DROP_CORRUPTED) && buf->error) {\n\t\tuvc_queue_buffer_requeue',
       '\tif ((queue->flags & UVC_QUEUE_DROP_CORRUPTED) && buf->error) {\n\t\tedgevision_trace_buffer(queue, buf, 1);\n\t\tuvc_queue_buffer_requeue')
    q=once(q,'\tvb2_set_plane_payload(&buf->buf.vb2_buf, 0, buf->bytesused);\n\tvb2_buffer_done',
       '\tvb2_set_plane_payload(&buf->buf.vb2_buf, 0, buf->bytesused);\n\tedgevision_trace_buffer(queue, buf, 0);\n\tvb2_buffer_done')
    new['uvc_queue.c']=q
    header=pathlib.Path(__file__).resolve().parents[1]/'kernel/uvc_trace.h'
    new['uvc_trace.h']=header.read_text(encoding='utf-8')
    diffs=[]
    for name in sorted(new):
        if old.get(name,'')==new[name]: continue
        prefix='drivers/media/usb/uvc/'
        diffs.extend(difflib.unified_diff(old.get(name,'').splitlines(keepends=True),new[name].splitlines(keepends=True),
                     fromfile='a/'+prefix+name if name in old else '/dev/null',tofile='b/'+prefix+name))
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(''.join(diffs),encoding='utf-8',newline='\n')
    print(json.dumps({'patch_sha256':hashlib.sha256(args.out.read_bytes()).hexdigest(),
                      'changed_files':[n for n in new if new[n]!=old.get(n,'')]}))


if __name__=='__main__': main()
