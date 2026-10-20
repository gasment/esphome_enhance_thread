'use strict';
(() => {
  const $ = id => document.getElementById(id);
  const match = location.pathname.match(/^\/thread_gateway\/node\/([a-zA-Z0-9_-]{1,32})$/);
  const nodeId = match ? match[1] : null;
  const base = nodeId ? '/thread_gateway/api/node/' + encodeURIComponent(nodeId) : null;
  const rows = new Map(), drafts = new Map();
  let nodeFilter = 'all', cachedNodes = [];
  const nodeFilters = [
    ['filterAll', 'all', '全部', () => true],
    ['filterReed', 'reed', 'REED节点', node => node.node_type === 'reed'],
    ['filterSed', 'sed', 'SED节点', node => node.node_type === 'sed'],
    ['filterOffline', 'offline', '离线节点', node => node.node_type === 'reed' && node.offline === true],
    ['filterUnconfigured', 'unconfigured', '未设置', node => node.active_entities === 0]
  ];
  let loading = false, saving = false, lastSchema = '', timer, rebooting = false;
  const modeNames = {offline:'离线',online:'常在线', awake:'短时唤醒', sleeping:'已声明休眠', unknown:'状态未知'};
  const statusNames = {queued:'待发送', waiting:'等待执行结果', success:'执行成功', failed:'执行失败', unconfirmed:'执行结果未确认'};
  const text = (element, value) => { element.textContent = value; };
  const valueText = value => value === null || value === undefined ? '未知' : typeof value === 'object' ? JSON.stringify(value) : String(value);
  const age = ms => ms === null || ms === undefined ? '尚未收到报文' : Math.floor(ms / 1000) + ' 秒前';
  function el(tag, className, content) {
    const e = document.createElement(tag);
    if (className) e.className = className;
    if (content !== undefined) text(e, content);
    return e;
  }
  function notice(message) { text($('notice'), message); }
  async function request(path, body) {
    const response = await fetch(path, {cache:'no-store', credentials:'same-origin',
      ...(body === undefined ? {} : {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)})});
    if (response.status === 401) throw new Error('认证失效，请刷新页面重新登录');
    let data;
    try { data = await response.json(); } catch (_) { throw new Error('网关响应无效（HTTP ' + response.status + '）'); }
    if (!response.ok || data.error) throw new Error(data.error || '请求失败：' + response.status);
    return data;
  }
  const bytes = value => value === undefined ? '未知' : value < 1024 ? value + ' B' : (value / 1024).toFixed(1) + ' KiB';
  function storagePair(used, total) {
    if (!Number.isFinite(used) || !Number.isFinite(total)) return ['— / —', 'KiB'];
    const format = value => (value / 1024).toFixed(value < 10240 ? 1 : 0);
    return [format(used) + ' / ' + format(total), 'KiB'];
  }
  function capacityCard(label, value, detail, tooltip) {
    const card = el('div', 'thread_gateway-stat');
    card.append(
      el('span', 'thread_gateway-stat-name', label[0]), el('strong', '', value),
      el('span', 'thread_gateway-stat-name', label[1]), el('small', '', detail)
    );
    card.title = tooltip || label.join('') + ': ' + value + ' (' + detail + ')';
    return card;
  }
  function summary(info) {
    const capacity = info.capacity, approved = info.approved, storage = info.nvs || {};
    $('summary').replaceChildren(
      el('strong', '', '网关 ' + info.gateway_id),
      el('span', 'thread_gateway-badge ' + (info.attached ? 'ok' : 'warn'), 'Thread ' + (info.attached ? '已连接' : '未连接')),
      el('span', 'thread_gateway-badge', 'HA 已生效 ' + info.active_entities + ' 个实体 / ' + info.active_devices + ' 个设备')
    );
    const cards = $('capacitySummary');
    cards.replaceChildren();
    for (const [key, label] of [['sensors','数值'],['binary_sensors','布尔'],['text_sensors','文本']]) {
      cards.append(capacityCard([label, '传感器'], approved[key] + ' / ' + capacity[key], '已批准 / 容量'));
    }
    for (const [key, label] of [
      ['switches',['开关','控制']], ['numbers',['数值','控制']],
      ['selects',['下拉','选择']], ['buttons',['按钮','控制']],
      ['texts',['文本','输入']], ['lights',['灯光','控制']],
      ['fans',['风扇','控制']], ['covers',['窗帘','控制']],
      ['climates',['温控','控制']], ['locks',['门锁','控制']], ['valves',['阀门','控制']]
    ]) {
      cards.append(capacityCard(label, (approved[key] || 0) + ' / ' + (capacity[key] || 0), '已批准 / 容量'));
    }
    const registry = storagePair(storage.registry_bytes, storage.limit_bytes);
    const partition = storagePair(storage.used_entry_bytes, storage.partition_bytes);
    cards.append(capacityCard(['节点', '清单'], registry[0], registry[1] + ' 当前/上限',
      '当前 ' + bytes(storage.registry_bytes) + '；上限 ' + bytes(storage.limit_bytes) + '；已保存 ' + bytes(storage.saved_bytes) + '；配置上限不是预留分区空间'));
    cards.append(capacityCard(['NVS', '分区'],
      partition[0], partition[1] + ' 已用/总量',
      storage.stats_available ? '所有组件共享；空闲条目空间 ' + bytes(storage.free_entry_bytes) + '；保存需额外写入空间' : 'NVS 统计暂不可用'));
    const messages = [];
    if (info.pending_restart) messages.push('有待重启应用的清单变更。保存后点击“重启并应用清单”。');
    if (storage.over_limit) messages.push('当前清单超过 capacity.nvs 上限，无法保存；请扩大上限或减少节点。');
    else if (storage.limit_bytes && storage.registry_bytes >= storage.limit_bytes * 0.9)
      messages.push('节点清单已达到配置上限的 90%，请关注剩余容量。');
    if (storage.stats_available && storage.registry_bytes > storage.free_entry_bytes)
      messages.push('共享 NVS 的空闲条目空间不足以容纳整份新清单，更新可能失败；清单上限不代表保证可写容量。');
    if (info.registry_error) messages.push('存储/注册错误：' + info.registry_error);
    text($('systemNotice'), messages.join('\n'));
    $('apply').disabled = rebooting || saving || info.reboot_pending;
  }
  function renderNodes(nodes) {
    cachedNodes = nodes;
    for (const [id, key, label, matches] of nodeFilters) {
      const button = $(id), selected = nodeFilter === key;
      button.className = 'btn btn-sm' + (selected ? ' btn-primary' : '');
      button.setAttribute('aria-pressed', String(selected));
      text(button, label + ' (' + nodes.filter(matches).length + ')');
    }
    const matches = nodeFilters.find(([, key]) => key === nodeFilter)[3];
    const visible = nodes.filter(matches);
    $('nodes').replaceChildren();
    if (!visible.length) $('nodes').append(el('p', 'thread_gateway-meta',
      nodes.length ? '没有符合当前筛选条件的节点。' : '暂未发现节点。等待节点上报实体清单。'));
    for (const node of visible) {
      const row = el('a', 'thread_gateway-row');
      row.href = '/thread_gateway/node/' + encodeURIComponent(node.id);
      const label = el('div');
      label.append(el('div', 'thread_gateway-title', '[' + (node.node_type || 'sed').toUpperCase() + '] ' + node.name + ' (' + node.id + ')'));
      label.append(el('div', 'thread_gateway-meta', node.entity_count + ' 个实体 · HA 已生效 ' +
        node.active_entities + ' 个 · 最近上报 ' + age(node.last_seen_ms)));
      row.append(label, el('span', '', modeNames[node.mode] || node.mode));
      $('nodes').append(row);
    }
  }
  async function sendControl(entity, input, button) {
    button.disabled = true;
    try {
      const body = {entity:entity.id, operation:entity.type === 'button' ? 'press' : 'set'};
      if (entity.type !== 'button') {
        body.value = entity.type === 'switch' ? input.checked : entity.type === 'number' ? Number(input.value) : input.value;
        if (['light','fan','cover','climate','lock','valve'].includes(entity.type)) body.value = JSON.parse(input.value);
        if (entity.type === 'number' && (!input.value.trim() || !Number.isFinite(body.value))) throw new Error('请输入有效数值');
        if (typeof body.value === 'string' && new TextEncoder().encode(body.value).length > entity.max_length)
          throw new Error('输入超过 UTF-8 字节长度限制');
      }
      const result = await request(base + '/command', body);
      const row = rows.get(entity.id);
      if (row) row.controlDirty = false;
      notice('命令 ' + result.command_id + ' 已提交，等待节点执行结果');
      await refresh();
    } catch (error) { notice(error.message); }
    finally { button.disabled = false; }
  }
  function createRow(entity) {
    const row = el('div', 'thread_gateway-row thread_gateway-entity-row');
    const label = el('div', 'thread_gateway-title thread_gateway-entity-name', entity.name + ' (' + entity.id + ')');
    const value = el('div', 'thread_gateway-value thread_gateway-entity-state');
    row.append(label, value);
    let input, button, approved, approvalInput;
    if (entity.readonly || entity.exposable) {
      const area = el('label', 'thread_gateway-control thread_gateway-registration');
      approvalInput = document.createElement('input'); approvalInput.type = 'checkbox';
      approvalInput.setAttribute('aria-label', '接入 HA：' + entity.name);
      approvalInput.checked = drafts.has(entity.id) ? drafts.get(entity.id) : entity.approved;
      approvalInput.addEventListener('change', () => drafts.set(entity.id, approvalInput.checked));
      approved = el('span', 'thread_gateway-meta');
      area.append(approvalInput, approved); row.append(area);
    } else {
      row.append(el('span', 'thread_gateway-meta thread_gateway-registration', '仅支持 Web 控制'));
    }
    if (!entity.readonly) {
      const area = el('div', 'thread_gateway-control thread_gateway-entity-controls');
      if (entity.type !== 'button') {
        input = document.createElement(entity.type === 'select' ? 'select' : 'input');
        input.setAttribute('aria-label', entity.name);
        if (entity.type === 'select') for (const option of entity.options) {
          const item = document.createElement('option'); item.value = option; text(item, option); input.append(item);
        } else if (entity.type === 'switch') input.type = 'checkbox';
        else if (entity.type === 'number') {
          input.type = 'number'; input.min = entity.min; input.max = entity.max; input.step = entity.step;
        } else { input.type = 'text'; input.maxLength = entity.max_length; }
        if (entity.type === 'switch') input.checked = entity.value === true;
        else if (entity.value !== null) input.value = typeof entity.value === 'object' ? JSON.stringify(entity.value) : entity.value;
        if (['light','fan','cover','climate','lock','valve'].includes(entity.type)) {
          input.maxLength = 900;
          input.value = '{}';
          input.placeholder = 'JSON 控制参数，例如 {"position":0.5}';
          input.title = JSON.stringify(entity.capabilities || {});
        }
        area.append(input);
      }
      button = el('button', 'btn btn-primary btn-sm', entity.type === 'button' ? '执行' : '设置');
      button.addEventListener('click', () => sendControl(entity, input, button));
      area.append(button); row.append(area);
    }
    $('entities').append(row);
    const rowState = {value, input, button, approved, approvalInput, controlDirty:false};
    if (input && !entity.readonly) {
      for (const event of ['input', 'change']) input.addEventListener(event, () => {rowState.controlDirty = true;});
    }
    return rowState;
  }
  function renderNode(node) {
    $('deleteNode').disabled = saving || rebooting;
    $('deleteNode').title = '删除节点记录，需二次确认；节点再次上报时可重新发现';
    text($('title'), node.name + ' · ' + node.id);
    text($('subtitle'), (node.node_type || 'sed').toUpperCase() + ' · ' + (modeNames[node.mode] || node.mode) + ' · 最近上报 ' + age(node.last_seen_ms) +
      ' · ' + (node.control_ready ? '允许 Web 控制' : '尚未进入可控制模式'));
    // Rebuild only when descriptors change. Polls preserve unsaved selections and control inputs.
    const schema = JSON.stringify(node.entities.map(e => [e.id,e.type,e.name,e.unit,e.min,e.max,e.step,e.options,e.max_length,e.exposable,e.capabilities]));
    if (schema !== lastSchema) {
      lastSchema = schema; rows.clear();
      $('entities').replaceChildren();
      for (const entity of node.entities) rows.set(entity.id, createRow(entity));
    }
    for (const entity of node.entities) {
      const row = rows.get(entity.id);
      let state;
      if (entity.type === 'button') {
        const latest = [...node.commands].reverse().find(command => command.entity === entity.id);
        state = latest ? '最近按下请求：#' + latest.id + ' · ' + (statusNames[latest.status] || latest.status) +
          (latest.error ? ' · ' + latest.error : '') : '暂无按下记录';
      } else {
        state = entity.type === 'switch' && typeof entity.value === 'boolean' ? (entity.value ? '开启' : '关闭') : valueText(entity.value);
      }
      text(row.value, state + (entity.unit ? ' ' + entity.unit : '') +
        (entity.value_age_ms === undefined ? '' : ' · ' + age(entity.value_age_ms)));
      if (row.approvalInput) {
        if (!drafts.has(entity.id)) row.approvalInput.checked = entity.approved;
        row.approvalInput.disabled = saving || (!entity.supported_metadata && !entity.approved);
        text(row.approved, (entity.approved ? '已批准' : '未批准') + ' · ' +
          (entity.active ? 'HA 已生效' : 'HA 未生效') +
          (entity.entity_category === 'diagnostic' ? ' · 诊断' : entity.entity_category === 'config' ? ' · 配置' : '') + (!entity.supported_metadata ? ' · 元数据不受当前固件支持' : ''));
      }
      if (!entity.readonly) {
        row.button.disabled = !node.control_ready || saving;
        if (row.input) {
          row.input.disabled = !node.control_ready;
          // Keep drafts while typing; otherwise display the latest reported state.
          if (!row.controlDirty && document.activeElement !== row.input && entity.value !== null && entity.value !== undefined) {
            if (entity.type === 'switch') row.input.checked = entity.value === true;
            else if (['number','select','input_text'].includes(entity.type)) row.input.value = String(entity.value);
          }
        }
      }
    }
    text($('commands'), node.commands.length ? node.commands.slice(-5).reverse().map(c =>
      '#' + c.id + ' ' + c.entity + '：' + (statusNames[c.status] || c.status) +
      '（尝试 ' + c.attempts + ' 次）' + (c.error ? ' ' + c.error : '')).join('\n') : '暂无控制记录');
    const {entities,commands,last_packet,...debug} = node;
    text($('debug'), JSON.stringify(debug, null, 2) + '\n\n最近报文：\n' + (last_packet || '无'));
  }
  async function refresh() {
    if (loading || rebooting) return;
    loading = true;
    try {
      const info = await request('/thread_gateway/api/' + (nodeId ? 'info' : 'nodes'));
      summary(info);
      if (nodeId) renderNode(await request(base));
      else renderNodes(info.nodes);
    } catch (error) { $('deleteNode').disabled = saving || rebooting; notice(error.message); }
    finally { loading = false; }
  }
  for (const [id, key] of nodeFilters) $(id).addEventListener('click', () => {
    nodeFilter = key;
    renderNodes(cachedNodes);
  });
  $('refresh').addEventListener('click', refresh);
  $('save').addEventListener('click', async () => {
    if (saving) return;
    saving = true; $('save').disabled = true;
    try {
      const entities = [...rows].filter(([,row]) => row.approved && row.approvalInput.checked).map(([id]) => id);
      await request(base + '/registration', {entities});
      drafts.clear(); notice('清单已存入 NVS。重启网关后应用 HA 实体清单。');
    } catch (error) { notice(error.message); }
    finally { saving = false; $('save').disabled = false; await refresh(); }
  });
  $('deleteNode').addEventListener('click', async () => {
    if (!nodeId || saving || rebooting) return;
    if (!confirm('确定删除节点“' + nodeId + '”？\n将清空该节点的清单、接入设置、当前读数和控制记录。\n下次网关重启后按最新批准清单应用 HA 实体。\n节点再次上报时会重新发现，原接入设置不恢复。')) return;
    saving = true;
    $('deleteNode').disabled = true; $('save').disabled = true; $('apply').disabled = true;
    try {
      await request(base + '/delete', {confirm:true});
      rebooting = true; clearInterval(timer);
      location.assign('/thread_gateway?deleted=1');
    } catch (error) {
      notice(error.message);
      saving = false; $('deleteNode').disabled = false; $('save').disabled = false;
      await refresh();
    }
  });
  $('apply').addEventListener('click', async () => {
    if (saving || rebooting) return;
    if (drafts.size) { notice('请先保存接入选择，再重启应用。'); return; }
    if (!confirm('将重启网关并应用已保存的实体清单，网络会短暂中断。继续？')) return;
    try {
      await request('/thread_gateway/api/apply', {confirm:true});
      rebooting = true; clearInterval(timer); $('apply').disabled = true;
      notice('网关正在重启，稍后刷新页面。');
    } catch (error) { notice(error.message); }
  });
  if (nodeId) {
    $('nodesSection').hidden = true; $('back').hidden = false; $('deleteNode').hidden = false;
    for (const id of ['entitiesSection','debugSection']) $(id).hidden = false;
  }
  if (new URLSearchParams(location.search).get('deleted') === '1')
    notice('节点已删除并保存至 NVS。下次网关重启应用最新清单；当前不会自动重启；若节点再次上报，会作为新节点重新加入管理。');
  refresh(); timer = setInterval(refresh, 1000);
})();
