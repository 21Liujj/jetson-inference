/* SPDX-License-Identifier: GPL-2.0-or-later */
/* EdgeVision UVC frame-boundary diagnostics for the frozen L4T 5.15 driver. */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM edgevision_uvc

#if !defined(_EDGEVISION_UVC_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _EDGEVISION_UVC_TRACE_H
#include <linux/tracepoint.h>

TRACE_EVENT(uvc_frame,
 TP_PROTO(const struct uvc_streaming *stream, const struct uvc_buffer *buf,
          u64 stream_id, u32 reason, u64 completed_ns),
 TP_ARGS(stream, buf, stream_id, reason, completed_ns),
 TP_STRUCT__entry(
  __string(usb, dev_name(&stream->dev->udev->dev))
  __field(int, interface)
  __field(int, video_minor)
  __field(u64, stream_id)
  __field(u32, index)
  __field(u32, sequence)
  __field(u32, bytesused)
  __field(u32, state)
  __field(u32, error)
  __field(u32, reason)
  __field(u64, completed_ns)
 ),
 TP_fast_assign(
  __assign_str(usb, dev_name(&stream->dev->udev->dev));
  __entry->interface = stream->intfnum;
  __entry->video_minor = stream->vdev.minor;
  __entry->stream_id = stream_id;
  __entry->index = buf->buf.vb2_buf.index;
  __entry->sequence = buf->buf.sequence;
  __entry->bytesused = buf->bytesused;
  __entry->state = buf->state;
  __entry->error = buf->error;
  __entry->reason = reason;
  __entry->completed_ns = completed_ns;
 ),
 TP_printk("usb=%s interface=%d video_minor=%d stream_id=%llu index=%u sequence=%u bytesused=%u state=%u error=%u reason=%u completed_ns=%llu",
  __get_str(usb), __entry->interface, __entry->video_minor,
  __entry->stream_id, __entry->index, __entry->sequence, __entry->bytesused,
  __entry->state, __entry->error, __entry->reason, __entry->completed_ns)
);

TRACE_EVENT(uvc_stream,
 TP_PROTO(const struct uvc_streaming *stream, u64 stream_id, u32 action,
          int result, u64 mono_ns),
 TP_ARGS(stream, stream_id, action, result, mono_ns),
 TP_STRUCT__entry(
  __string(usb, dev_name(&stream->dev->udev->dev))
  __field(int, interface)
  __field(int, video_minor)
  __field(u64, stream_id)
  __field(u32, action)
  __field(int, result)
  __field(u64, mono_ns)
 ),
 TP_fast_assign(
  __assign_str(usb, dev_name(&stream->dev->udev->dev));
  __entry->interface = stream->intfnum;
  __entry->video_minor = stream->vdev.minor;
  __entry->stream_id = stream_id;
  __entry->action = action;
  __entry->result = result;
  __entry->mono_ns = mono_ns;
 ),
 TP_printk("usb=%s interface=%d video_minor=%d stream_id=%llu action=%u result=%d mono_ns=%llu",
  __get_str(usb), __entry->interface, __entry->video_minor, __entry->stream_id,
  __entry->action, __entry->result, __entry->mono_ns)
);
#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE uvc_trace
#include <trace/define_trace.h>
