'use strict';
const base='https://github.com/21Liujj/jetson-inference/blob/edgevision/v3.1/';
const stages=[
 ['USB / UVC / V4L2','M2–M4 基础验收完成','直接 V4L2/MMAP、100 次启停、30 分钟采集和真实拔插恢复已验。UVC 新增帧/stream 事件，真实恢复测试中 1,803 条消费记录全部关联；测试后实际回退原模块。','关键约束：相机单一 owner；sequence 缺口与应用丢弃分别统计。','projects/edgevision/docs/m4-uvc-trace.md'],
 ['CUDA / TensorRT','数值与长窗口对照通过 / 资源对照待补','48 例 CUDA 预处理、200 图模型回归及真实 GPU 生命周期通过。五配置各 3×300 秒，共 646,305 次完成；Pinned 离线中位 P0 74.09、P1 129.72、P2 192.68 images/s。P2 吞吐更高，尾延迟比 P1 差；利用率、RSS 和功耗对照待补。','关键约束：运行时使用 P1；P2 为离线组件对照，不能当摄像头帧率。','projects/edgevision/docs/m5-gpu-pipeline.md'],
 ['ROS2 / Humble','M6 功能通过 / 正式性能待补','三个 Humble 包已在 Jetson 构建。真实图像、同帧检测、诊断、服务、Action 取消/超时、慢订阅与受限 bag 回放已验；安装时加载旧动态库的问题和修复保留原始证据。四组短窗口开销不替代正式性能。','关键约束：ROS2 与其他入口共享任务；ROS 发布时钟和采集单调时钟分开。','projects/edgevision/docs/m6-ros2.md'],
 ['本地 VLM / VisionAgent','M7 集成验证中','Qwen3-VL-2B 已完成独立真实图像问答和模型工具选择。共享快照、进程管理、ROS Action 与 Python Agent 正在板端集成；完整任务集、三条真实轨迹、资源影响和故障验收仍待完成。','关键约束：模型真实参与；参数按 schema 校验，回答引用实际工具结果。','projects/edgevision/docs/progress.md']
];
const tabs=Array.from(document.querySelectorAll('[data-stage]'));
function selectTab(button){
 tabs.forEach(item=>{item.setAttribute('aria-selected',String(item===button));item.tabIndex=item===button?0:-1});
 const [title,state,description,contract,path]=stages[Number(button.dataset.stage)];
 document.getElementById('stage-title').textContent=title;
 document.getElementById('stage-state').textContent=state;
 document.getElementById('stage-description').textContent=description;
 document.getElementById('stage-contract').textContent=contract;
 document.getElementById('stage-source').href=base+path;
 document.getElementById('module-detail').setAttribute('aria-labelledby',button.id);
}
tabs.forEach((button,index)=>{
 button.addEventListener('click',()=>selectTab(button));
 button.addEventListener('keydown',event=>{
  let next;
  if(event.key==='ArrowRight')next=(index+1)%tabs.length;
  if(event.key==='ArrowLeft')next=(index+tabs.length-1)%tabs.length;
  if(event.key==='Home')next=0;
  if(event.key==='End')next=tabs.length-1;
  if(next!==undefined){event.preventDefault();selectTab(tabs[next]);tabs[next].focus()}
 });
});
document.querySelectorAll('[data-evidence]').forEach(link=>{link.href=base+'projects/edgevision/docs/'+(link.dataset.evidence==='progress'?'progress.md':'baseline.md')});
document.getElementById('copy-command').addEventListener('click',async()=>{
 const feedback=document.getElementById('copy-feedback');
 try{await navigator.clipboard.writeText(document.getElementById('build-command').textContent);feedback.textContent='构建命令已复制。'}
 catch{feedback.textContent='浏览器未允许复制，请直接选择上方命令。'}
});
document.querySelectorAll('nav a').forEach(link=>link.addEventListener('click',()=>{document.querySelectorAll('nav a').forEach(item=>item.classList.remove('selected'));link.classList.add('selected')}));
