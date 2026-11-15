'use strict';
const base='https://github.com/21Liujj/jetson-inference/blob/edgevision/v3.1/';
const stages=[
 ['USB / UVC / V4L2','M2 部分验收','直接 V4L2/MMAP、eventfd 停止与公共控制入口已实测；100 次启停包含 3 次受限启动重试。运行中拔插、长采集和 UVC trace 仍待验收。','关键约束：相机单一 owner；sequence 缺口与应用丢弃分别统计。','projects/edgevision/src/camera/v4l2_capture.cc'],
 ['CUDA / TensorRT','M1 基线 / M5 待验收','已核对 CUDA 12.6 与 TensorRT 10.3，原样构建上游推理应用。后续优化要求数值回归、内存路径对照、事件流水线与 Nsight 时间线。','关键约束：提交成功不等于 GPU 完成；在途 buffer 保留到完成事件。','c/tensorNet.cpp'],
 ['ROS2 / Humble','M6 待验收','板端已安装 ROS2 Humble。图像、检测、帧元数据、诊断、启停服务和任务 Action 是正式交付范围；安装依赖不表示集成通过。','关键约束：相机只有一个 owner；ROS 时间域与采集单调时钟分别记录。','UPSTREAM.md'],
 ['本地 VLM / VisionAgent','M7 待验收','后续独立核验既有 EdgeQwen 模型接口，再接入结构化计划、受约束工具与真实任务轨迹。规则测试不能替代模型参与的正式验收。','关键约束：Agent 与 ROS2 共享任务管理；回答引用真实工具结果。','projects/edgevision/docs/spec/construction-plan-v3.1.md#11-视觉任务agent正式开发']
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
