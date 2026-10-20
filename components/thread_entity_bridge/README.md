# thread_entity_bridge

thread_reed_transporter 与 thread_gateway 共用的官方实体适配层。

节点通过 bind(id, source) 获得元数据读取、状态读取、标准控制入口和状态订阅；网关通过 make_proxy 创建相同类型的代理。代理字符串和 traits 具有固件生命周期，避免清单临时 JSON 销毁后产生悬挂指针。

仅 USE_THREAD_GATEWAY 构建网关代理部分。节点端不需要 USE_DEVICES，也不会注册副本实体。所有 API 改写均由 thread_gateway/api_compat.py 在项目 temp 中生成，不修改安装包。

详细使用和资源边界见 ../thread_reed_transporter/README.md。

本组件为内部共享实体适配层，由 `thread_gateway` 和 `thread_reed_transporter` 自动加载，不需要独立的顶层 YAML 配置；本地 external_components 使用组件白名单时仍需列出它。
