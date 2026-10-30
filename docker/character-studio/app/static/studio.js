const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
const clone = (value) => structuredClone(value);
const bytes = (value) => new TextEncoder().encode(value || '').length;
const authorKeys = ['role', 'faction', 'age', 'home', 'traits', 'values', 'goals', 'fears', 'relationships', 'notes'];
const appearanceFields = [['skin', 'Odstín kůže', 'skins'], ['face', 'Obličej', 'faces'], ['hair_style', 'Účes', 'hair_styles'], ['hair_color', 'Barva vlasů', 'hair_colors'], ['facial_hair', 'Vousy / doplňky', 'facial_hairs']];
const tabs = ['appearance', 'personality', 'story', 'export'];
const defaults = () => ({card: {agent_id: '', spawn_id: '', map_id: 725, name: '', backstory: '', personality: ''}, appearance: {race: 1, gender: 0, class_id: 1, skin: 0, face: 0, hair_style: 0, hair_color: 0, facial_hair: 0, equipment: []}, authoring: Object.fromEntries(authorKeys.map(key => [key, '']))});
let catalog = {races: [], equipment: [], equipment_slots: [], source_profiles: [], targets: []};
let characters = [];
let current = defaults();
let dirty = false;
let busy = false;
let csrf = '';
let activeTab = 'appearance';
let preview;
let previewKey = '';
let previewTimer;
let toastTimer;
let generationDocumentId;
let selectionVersion = 0;
let generationVersion = 0;

function el(tag, text, className) {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (className) node.className = className;
  return node;
}

function toast(message, error = false) {
  const node = $('#toast');
  clearTimeout(toastTimer);
  node.textContent = message;
  node.classList.toggle('error', error);
  node.hidden = false;
  toastTimer = setTimeout(() => { node.hidden = true; }, error ? 6500 : 3500);
}

function errorText(data) {
  if (typeof data?.detail === 'string') return data.detail;
  if (Array.isArray(data?.detail)) return data.detail.map(item => `${(item.loc || []).slice(1).join('.')}: ${item.msg}`).join('; ');
  return 'Požadavek se nepodařilo dokončit. Zkontroluj spojení se serverem.';
}

async function api(path, options = {}) {
  const method = options.method || 'GET';
  const headers = {'Accept': 'application/json', ...(options.headers || {})};
  if (method !== 'GET') {
    headers['X-CSRF-Token'] = csrf;
    headers['Content-Type'] = 'application/json';
  }
  let response;
  try { response = await fetch(path, {...options, headers, credentials: 'same-origin', cache: 'no-store'}); }
  catch { throw new Error('Server ateliéru není dostupný. Návrh v otevřeném okně zůstává zachovaný.'); }
  let data;
  try { data = await response.json(); } catch { throw new Error('Server neposlal platnou odpověď.'); }
  if (!response.ok) throw new Error(response.status === 409 ? 'Tuto kartu mezitím změnil někdo jiný. Zálohuj si návrh a obnov stránku před dalším uložením.' : errorText(data));
  return data;
}

async function task(fn) {
  if (busy) return;
  busy = true;
  $('#save-character').disabled = true;
  $('#character-form').inert = true;
  $('.library').inert = true;
  $('#duplicate-character').disabled = true;
  try { return await fn(); }
  catch (error) { toast(error.message, true); }
  finally {
    busy = false;
    $('#save-character').disabled = false;
    $('#character-form').inert = false;
    $('.library').inert = false;
    $('#duplicate-character').disabled = false;
  }
}

function race() { return catalog.races.find(item => item.id === current.appearance.race); }
function variant() { return race()?.variants.find(item => item.gender === current.appearance.gender); }
function slotId(slot) { return typeof slot === 'string' ? slot : slot.id; }
function slotName(slot) { return typeof slot === 'string' ? slot : slot.name; }

function choices(field) {
  const row = variant();
  if (!row) return [0];
  if (field === 'face') return row.face_options?.find(item => item.skin === current.appearance.skin)?.faces || row.faces || [0];
  if (field === 'hair_color') {
    const hair = row.hair_options?.find(item => item.style === current.appearance.hair_style)?.colors || row.hair_colors || [0];
    const facial = row.facial_options?.find(item => item.style === current.appearance.facial_hair)?.colors || row.hair_colors || [0];
    return hair.filter(color => facial.includes(color));
  }
  const key = appearanceFields.find(row => row[0] === field)?.[2];
  return row[key] || [0];
}

function normalizeAppearance() {
  if (!race() && catalog.races.length) current.appearance.race = catalog.races[0].id;
  for (const field of ['skin', 'face', 'hair_style', 'facial_hair', 'hair_color']) {
    const available = choices(field);
    if (!available.includes(current.appearance[field])) current.appearance[field] = available[0] ?? 0;
  }
}

function renderAppearance() {
  const select = $('#appearance-race');
  select.replaceChildren(...catalog.races.map(item => {
    const option = el('option', item.name);
    option.value = item.id;
    option.selected = current.appearance.race === item.id;
    return option;
  }));
  $$('input[name="gender"]').forEach(input => { input.checked = Number(input.value) === current.appearance.gender; });
  const controls = $('#appearance-controls');
  controls.replaceChildren();
  for (const [field, label] of appearanceFields) {
    const available = choices(field);
    const box = el('div', undefined, 'appearance-control');
    const caption = el('label', label);
    const counter = el('small', `${available.length} variant`);
    caption.append(counter);
    const stepper = el('div', undefined, 'stepper');
    const before = el('button', '−');
    const after = el('button', '+');
    before.type = after.type = 'button';
    before.setAttribute('aria-label', `${label}: předchozí varianta`);
    after.setAttribute('aria-label', `${label}: další varianta`);
    const output = el('output', available.length ? String(current.appearance[field] + 1).padStart(2, '0') : '—');
    output.setAttribute('aria-label', `${label}: ${current.appearance[field] + 1}`);
    caption.htmlFor = `appearance-${field}`;
    output.id = `appearance-${field}`;
    for (const [button, offset] of [[before, -1], [after, 1]]) {
      button.disabled = available.length < 2;
      button.addEventListener('click', () => {
        current.appearance[field] = available[(available.indexOf(current.appearance[field]) + offset + available.length) % available.length];
        normalizeAppearance();
        renderAppearance();
        changed();
      });
    }
    stepper.append(before, output, after);
    box.append(caption, stepper);
    controls.append(box);
  }
}

function renderEquipment() {
  const query = $('#equipment-search').value.trim().toLocaleLowerCase('cs');
  $('#equipment-controls').replaceChildren(...catalog.equipment_slots.map(slot => {
    const id = slotId(slot);
    const label = el('label', slotName(slot), 'field');
    const select = el('select');
    select.setAttribute('aria-label', slotName(slot));
    const empty = el('option', 'Bez výbavy');
    empty.value = '';
    select.append(empty);
    const selected = current.appearance.equipment.find(item => item.slot === id);
    for (const item of catalog.equipment.filter(item => item.slot === id && (!query || item.item_id === selected?.item_id || `${item.label} ${item.item_id}`.toLocaleLowerCase('cs').includes(query)))) {
      const option = el('option', `${item.label} · ${item.item_id}`);
      option.value = String(item.item_id);
      option.selected = item.item_id === selected?.item_id;
      select.append(option);
    }
    select.addEventListener('change', () => {
      current.appearance.equipment = current.appearance.equipment.filter(item => item.slot !== id);
      const selected = catalog.equipment.find(item => item.slot === id && String(item.item_id) === select.value);
      if (selected) current.appearance.equipment.push({slot: id, item_id: selected.item_id, display_id: selected.display_id});
      changed();
    });
    label.append(select);
    return label;
  }));
}

function readForm() {
  $$('[data-card]').forEach(input => {
    const key = input.dataset.card;
    current.card[key] = key === 'map_id' ? (input.value === '' ? 725 : Number(input.value)) : input.value;
  });
  current.card.agent_id = current.card.spawn_id;
  $$('[data-author]').forEach(input => { current.authoring[input.dataset.author] = input.value; });
}

function renderForm() {
  $$('[data-card]').forEach(input => { input.value = current.card[input.dataset.card] ?? ''; });
  $$('[data-author]').forEach(input => {
    const value = current.authoring[input.dataset.author] || '';
    if (input.tagName === 'SELECT' && value && ![...input.options].some(option => option.value === value)) {
      const option = el('option', value); option.value = value; input.append(option);
    }
    input.value = value;
  });
  renderAppearance();
  renderEquipment();
  renderSummary();
}

function validId(value) {
  if (!/^[1-9][0-9]{0,19}$/.test(value)) return false;
  try { return BigInt(value) <= 18446744073709551615n; } catch { return false; }
}

function completion() {
  return [!!current.card.name.trim() && current.card.name.length <= 32 && bytes(current.card.name) <= 80,
    !!current.card.personality.trim() && bytes(current.card.personality) <= 2048,
    !!current.card.backstory.trim() && bytes(current.card.backstory) <= 8192,
    targetStatus().eligible];
}

function targetStatus() {
  const registry = catalog.target_registry;
  if (registry?.available !== true) return {eligible: false, reason: registry?.reason || 'Nabídku řízených NPC se nepodařilo ověřit. Návrh můžeš uložit, herní export zatím není dostupný.'};
  if (!current.card.spawn_id) return {eligible: false, reason: 'Vyber cílové NPC. Nové číslo samo o sobě postavu ve světě nevytvoří ani nezapne její LLM řízení.'};
  if (!validId(current.card.spawn_id) || current.card.agent_id !== current.card.spawn_id) return {eligible: false, reason: 'Agent ID a Spawn ID musí být stejné platné číslo.'};
  const row = (catalog.targets || []).find(row => row.spawn_id === current.card.spawn_id);
  if (!row) return {eligible: false, reason: 'Toto ID není v ověřené populaci světa. Návrh zůstává uložený; před herním exportem zvol existující řízené NPC.'};
  if (row.map_id !== current.card.map_id) return {eligible: false, reason: `Cílové NPC patří na mapu ${row.map_id}. Zvolená mapa s ním nesouhlasí.`};
  if (row.agent_id !== current.card.agent_id || row.eligible !== true) return {eligible: false, reason: row.reason || 'Toto NPC není registrované pro řízení LLM. Samotný import charakterové karty jeho řízení nezapne.'};
  const installed = catalog.source_profiles.find(source => source.card.spawn_id === row.spawn_id && source.card.map_id === row.map_id);
  let reason = `Cíl ${row.spawn_id} je ověřený podle aktivního profilu ${registry.profile}.`;
  if (installed && installed.card.name !== current.card.name) reason += ` Používá kartu „${installed.card.name}“. Nasazení tohoto exportu ji nahradí novou identitou.`;
  return {eligible: true, reason};
}

function renderTargets() {
  const select = $('#target-select');
  const options = [];
  const placeholder = el('option', 'Vyber ověřené NPC…');
  placeholder.value = ''; options.push(placeholder);
  const targets = catalog.targets || [];
  const groups = new Map([[true, el('optgroup')], [false, el('optgroup')]]);
  groups.get(true).label = 'Řízená NPC – lze přiřadit kartu';
  groups.get(false).label = 'Původní AI – zatím nelze exportovat';
  for (const row of targets) {
    const source = catalog.source_profiles.find(source => source.card.spawn_id === row.spawn_id && source.card.map_id === row.map_id);
    const name = source?.card.name || row.name || row.role || 'NPC';
    const option = el('option', `${row.spawn_id} · ${name}${row.eligible ? '' : ' · není řízené LLM'}`);
    option.value = row.spawn_id;
    option.disabled = row.eligible !== true;
    option.selected = row.spawn_id === current.card.spawn_id;
    groups.get(row.eligible === true).append(option);
  }
  if (current.card.spawn_id && !targets.some(row => row.spawn_id === current.card.spawn_id)) {
    const unknown = el('option', `${current.card.spawn_id} · neověřený cíl`);
    unknown.value = current.card.spawn_id; unknown.selected = true; unknown.disabled = true; options.push(unknown);
  }
  for (const group of groups.values()) if (group.children.length) options.push(group);
  select.replaceChildren(...options);
  if (!current.card.spawn_id) select.value = '';
  select.disabled = catalog.target_registry?.available !== true;
  $('#target-registry-source').textContent = catalog.target_registry?.available ? `Zdroj: ověřený populační profil ${catalog.target_registry.profile}.` : 'Ověřený populační profil není dostupný.';
  const status = targetStatus();
  $('#target-status').textContent = status.reason;
  $('#target-status').classList.toggle('invalid', !status.eligible);
  for (const id of ['export-bundle', 'export-profile', 'export-appearance']) {
    $(`#${id}`).disabled = !status.eligible;
    $(`#${id}`).title = status.eligible ? '' : status.reason;
    $(`#${id}`).setAttribute('aria-describedby', 'target-status');
  }
}

function renderSummary() {
  const name = current.card.name.trim() || 'Nová postava';
  const raceName = race()?.name || 'Humanoid';
  const gender = current.appearance.gender === 1 ? 'žena' : 'muž';
  $('#character-title').textContent = $('#preview-name').textContent = name;
  $('#character-emblem').textContent = name[0].toLocaleUpperCase('cs');
  $('#character-subtitle').textContent = `${raceName} · ${current.authoring.role || 'Role čeká na příběh'}`;
  $('#preview-race').textContent = `${raceName} · ${gender}`;
  $('#preview-faction').textContent = current.authoring.faction || 'Bez příslušnosti';
  $('#preview-role').textContent = current.authoring.role || 'Role čeká na příběh';
  const story = current.card.backstory.trim().split(/\n/).find(Boolean);
  $('#preview-quote').textContent = story ? `„${story.slice(0, 165)}${story.length > 165 ? '…' : ''}“` : '„Každý, koho potkáš, má příběh. Tenhle teprve začíná.“';
  const state = $('#save-state');
  state.textContent = dirty ? 'Neuložené změny' : current.id ? 'Uloženo v ateliéru' : 'Neuložený návrh';
  state.classList.toggle('dirty', dirty);
  for (const [field, max] of [['personality', 2048], ['backstory', 8192]]) {
    const size = bytes(current.card[field]);
    const counter = $(`#${field}-counter`);
    counter.textContent = `${size.toLocaleString('cs')} / ${max.toLocaleString('cs')} bajtů`;
    counter.classList.toggle('over-limit', size > max);
  }
  const checklist = completion();
  const labels = ['Jméno postavy v herním limitu', 'Osobnost pro rozhodování a řeč', 'Životopis a vlastní minulost', 'Ověřený řízený cíl NPC a mapa'];
  $('#export-checklist').replaceChildren(...checklist.map((ready, i) => {
    const row = el('li', undefined, ready ? 'complete' : 'incomplete');
    row.append(el('span', ready ? '✓' : '·', 'check-mark'), el('span', labels[i]));
    return row;
  }));
  const count = checklist.filter(Boolean).length;
  $('#completion-label').textContent = `${count} / 4`;
  $('#completion-bar').style.width = `${count * 25}%`;
  $('#delete-character').hidden = !current.id;
  renderTargets();
  const key = JSON.stringify(current.appearance);
  if (key !== previewKey) {
    clearTimeout(previewTimer);
    previewTimer = setTimeout(() => {
      if (preview) { previewKey = key; preview.update(clone(current.appearance)); }
    }, 120);
  }
}

function changed() { dirty = true; renderSummary(); }

function renderLibrary() {
  $('#library-count').textContent = characters.length;
  const query = $('#character-search').value.trim().toLocaleLowerCase('cs');
  const filtered = characters.filter(item => `${item.card.name} ${item.authoring.role} ${item.authoring.faction}`.toLocaleLowerCase('cs').includes(query));
  const list = $('#character-list');
  list.replaceChildren();
  if (!filtered.length) list.append(el('p', query ? 'Tahle postava se v kronice zatím neskrývá.' : 'Začni novou postavou nebo načti Matěje či Bělu z projektu.', 'muted library-empty'));
  for (const item of filtered) {
    const button = el('button', undefined, `character-row${item.id === current.id ? ' selected' : ''}`);
    const name = item.card.name || 'Bezejmenná postava';
    const details = el('span', undefined, 'character-row-text');
    const raceName = catalog.races.find(row => row.id === item.appearance.race)?.name || 'Humanoid';
    details.append(el('strong', name), el('small', `${raceName} · ${item.authoring.role || 'Vlastní postava'}`));
    button.append(el('span', name[0].toLocaleUpperCase('cs'), 'mini-emblem'), details, el('span', '›', 'row-arrow'));
    button.setAttribute('aria-pressed', String(item.id === current.id));
    button.addEventListener('click', () => { if (item.id !== current.id && canLeave()) selectCharacter(item); });
    list.append(button);
  }
}

function canLeave() { return !busy && (!dirty || window.confirm('Opustit neuložené změny této postavy?')); }
function selectCharacter(doc) {
  ++selectionVersion;
  ++generationVersion;
  current = clone(doc);
  current.authoring = {...defaults().authoring, ...current.authoring};
  dirty = false;
  previewKey = '';
  renderForm();
  renderLibrary();
}

async function save() {
  readForm();
  const payload = {card: current.card, appearance: current.appearance, authoring: current.authoring};
  if (current.id) payload.revision = current.revision;
  const doc = await api(current.id ? `/api/characters/${current.id}` : '/api/characters', {method: current.id ? 'PUT' : 'POST', body: JSON.stringify(payload)});
  const index = characters.findIndex(item => item.id === doc.id);
  if (index >= 0) characters[index] = doc; else characters.unshift(doc);
  selectCharacter(doc);
  toast('Postava je uložená v ateliéru.');
  return doc;
}

function showTab(name) {
  activeTab = name;
  $$('.tab').forEach(button => { const active = button.dataset.tab === name; button.classList.toggle('active', active); button.setAttribute('aria-selected', String(active)); button.tabIndex = active ? 0 : -1; });
  $$('.tab-panel').forEach(panel => { panel.hidden = panel.id !== `panel-${name}`; });
  const next = tabs[tabs.indexOf(name) + 1];
  const labels = {personality: 'Pokračovat na charakter', story: 'Pokračovat na příběh', export: 'Připravit export'};
  $('#next-tab').hidden = !next;
  $('#next-tab').textContent = `${labels[next] || ''} →`;
  renderSummary();
}

async function download(path) {
  const response = await fetch(path, {credentials: 'same-origin', cache: 'no-store'});
  if (!response.ok) {
    const data = await response.json().catch(() => null);
    throw new Error(errorText(data));
  }
  const blob = await response.blob();
  const disposition = response.headers.get('content-disposition') || '';
  const filename = disposition.match(/filename="?([^";]+)"?/)?.[1] || 'postava.zip';
  downloadBlob(blob, filename);
}

function downloadBlob(blob, name) {
  const link = el('a');
  link.href = URL.createObjectURL(blob); link.download = name;
  document.body.append(link); link.click(); link.remove();
  setTimeout(() => URL.revokeObjectURL(link.href), 1500);
}

async function exportFile(kind) {
  readForm();
  if (!targetStatus().eligible) { showTab('export'); throw new Error(targetStatus().reason); }
  if (completion().some(ready => !ready)) { showTab('export'); throw new Error('Před exportem doplň jméno, osobnost, životopis a platnou identitu NPC.'); }
  if (dirty || !current.id) await save();
  await download(`/api/characters/${current.id}/${kind}`);
}

function setupEvents() {
  $('#character-form').addEventListener('submit', event => event.preventDefault());
  $('#character-form').addEventListener('input', event => {
    if (event.target.matches('[data-card],[data-author]')) { readForm(); changed(); }
  });
  $('#character-form').addEventListener('change', event => {
    if (event.target.matches('[data-author]')) { readForm(); changed(); }
  });
  $('#target-select').addEventListener('change', event => {
    const row = (catalog.targets || []).find(row => row.spawn_id === event.target.value && row.eligible === true);
    if (!row) return;
    current.card.spawn_id = row.spawn_id;
    current.card.agent_id = row.agent_id;
    current.card.map_id = row.map_id;
    $('#card-spawn').value = row.spawn_id;
    $('#card-map').value = row.map_id;
    changed();
  });
  $('#appearance-race').addEventListener('change', event => { current.appearance.race = Number(event.target.value); normalizeAppearance(); renderAppearance(); changed(); });
  $$('input[name="gender"]').forEach(input => input.addEventListener('change', () => { current.appearance.gender = Number(input.value); normalizeAppearance(); renderAppearance(); changed(); }));
  $('#randomize-appearance').addEventListener('click', () => {
    for (const field of ['skin', 'face', 'hair_style', 'facial_hair', 'hair_color']) {
      const values = choices(field); current.appearance[field] = values[Math.floor(Math.random() * values.length)] ?? 0;
    }
    normalizeAppearance(); renderAppearance(); changed();
  });
  $('#equipment-search').addEventListener('input', renderEquipment);
  $('#clear-equipment').addEventListener('click', () => { current.appearance.equipment = []; renderEquipment(); changed(); });
  $('#save-character').addEventListener('click', () => task(save));
  $('#new-character').addEventListener('click', () => { if (canLeave()) { selectCharacter(defaults()); showTab('appearance'); $('#card-name').focus(); } });
  $('#duplicate-character').addEventListener('click', () => {
    if (busy) return;
    readForm(); const copy = clone(current); delete copy.id; delete copy.revision;
    copy.card.name = `${copy.card.name.slice(0, 23)} (kopie)`.trim();
    copy.card.spawn_id = ''; copy.card.agent_id = '';
    selectCharacter(copy); changed(); toast('Kopie je připravená. Před exportem jí přiděl vlastní Spawn ID.');
  });
  $('#character-search').addEventListener('input', renderLibrary);
  $$('.tab').forEach(button => {
    button.addEventListener('click', () => showTab(button.dataset.tab));
    button.addEventListener('keydown', event => {
      if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
      event.preventDefault();
      const index = tabs.indexOf(activeTab);
      const next = event.key === 'Home' ? 0 : event.key === 'End' ? 3 : (index + (event.key === 'ArrowLeft' ? -1 : 1) + 4) % 4;
      showTab(tabs[next]); $(`#tab-${tabs[next]}`).focus();
    });
  });
  $('#next-tab').addEventListener('click', () => { const next = tabs[tabs.indexOf(activeTab) + 1]; if (next) showTab(next); });
  for (const [id, path] of [['export-bundle', 'bundle.zip'], ['export-profile', 'profile'], ['export-appearance', 'appearance']]) $( `#${id}`).addEventListener('click', () => task(() => exportFile(path)));
  $('#export-draft').addEventListener('click', () => { readForm(); downloadBlob(new Blob([JSON.stringify(current, null, 2) + '\n'], {type: 'application/json'}), 'charakter-navrh.json'); });
  $('#delete-character').addEventListener('click', () => {
    if (!current.id || !window.confirm('Smazat tento uložený návrh z ateliéru? Již exportované soubory ani herní NPC se nezmění.')) return;
    task(async () => {
      await api(`/api/characters/${current.id}?revision=${current.revision}`, {method: 'DELETE'});
      characters = characters.filter(item => item.id !== current.id);
      selectCharacter(characters[0] || defaults()); toast('Návrh byl smazaný.');
    });
  });
  $('#import-profile').addEventListener('click', () => { if (canLeave()) $('#import-file').click(); });
  $('#import-file').addEventListener('change', event => {
    const file = event.target.files[0]; event.target.value = '';
    if (!file) return;
    task(async () => {
      if (file.size > 65536) throw new Error('Importovaný návrh může mít nejvýše 64 KiB.');
      const text = await file.text();
      let draftFile = false;
      try { const value = JSON.parse(text); draftFile = !!(value?.card && value?.appearance); }
      catch { throw new Error('Soubor neobsahuje platný JSON.'); }
      if (!draftFile && file.size > 16384) throw new Error('Nativní karta NPC může mít nejvýše 16 KiB.');
      // Native uint64 IDs must reach Python unchanged; never parse/re-stringify in JS.
      const doc = await api(draftFile ? '/api/import-draft' : '/api/import-profile', {method: 'POST', body: text});
      characters.unshift(doc); selectCharacter(doc); toast('Karta byla importovaná do ateliéru.');
    });
  });
  $('#open-generator').addEventListener('click', () => {
    readForm(); generationDocumentId = selectionVersion; ++generationVersion;
    $('#generation-result').hidden = true; $('#generation-status').textContent = '';
    $('#generator-dialog').showModal();
  });
  $('#generate-story').addEventListener('click', async () => {
    const button = $('#generate-story'); if (button.disabled) return;
    readForm(); button.disabled = true;
    const selectedVersion = selectionVersion;
    const requestVersion = ++generationVersion;
    $('#generation-result').hidden = true;
    $('#generation-status').textContent = 'Qwen právě píše. Může to trvat až půl minuty…';
    try {
      const result = await api('/api/generate', {method: 'POST', body: JSON.stringify({card: current.card, appearance: current.appearance, authoring: current.authoring, instruction: $('#generation-instruction').value})});
      if (selectedVersion !== selectionVersion || requestVersion !== generationVersion) return;
      $('#proposal-backstory').value = result.backstory;
      $('#proposal-personality').value = result.personality;
      $('#generation-suggestions').replaceChildren(...(result.suggestions || []).map(item => el('li', item)));
      $('#generation-result').hidden = false;
      $('#generation-status').textContent = 'Návrh je připravený. Uprav ho a převezmi, až ti bude vyhovovat.';
    } catch (error) { if (selectedVersion === selectionVersion && requestVersion === generationVersion) $('#generation-status').textContent = error.message; }
    finally { button.disabled = false; }
  });
  $('#accept-generation').addEventListener('click', () => {
    if (generationDocumentId !== selectionVersion) { toast('Vybraná postava se změnila. Vygeneruj návrh pro aktuální postavu.', true); return; }
    current.card.backstory = $('#proposal-backstory').value;
    current.card.personality = $('#proposal-personality').value;
    $('#generator-dialog').close(); renderForm(); changed(); toast('Návrh byl převzatý. Zkontroluj ho a ulož postavu.');
  });
  $('#generator-dialog').addEventListener('close', () => { ++generationVersion; });
  $('#rotate-left').addEventListener('click', () => preview?.rotate?.(-0.3));
  $('#rotate-right').addEventListener('click', () => preview?.rotate?.(0.3));
  window.addEventListener('beforeunload', event => { if (dirty) { event.preventDefault(); event.returnValue = ''; } });
}

function setModelStatus(status) {
  if (typeof status === 'string') status = {state: 'loading', message: status};
  const ready = status.state === 'ready';
  $('#model-fallback').hidden = ready;
  $('#model-status').textContent = status.message || 'Náhled postavy';
  $('#model-status-detail').textContent = status.detail || 'Parametry vzhledu jsou součástí exportované karty.';
  $('#preview-render-note').textContent = ready ? (status.detail || 'Původní model 3.3.5a · statická póza. Výbavu ověř ve hře.') : '';
}

async function init() {
  setupEvents();
  const observer = new URL(location.href); observer.port = '9091'; observer.pathname = '/'; observer.search = ''; observer.hash = '';
  $('#observer-link').href = observer.href;
  try {
    const session = await api('/api/session'); csrf = session.csrf_token;
    const results = await Promise.all([api('/api/catalog'), api('/api/characters')]);
    catalog = results[0]; characters = results[1].characters;
    $('#source-list').replaceChildren(...catalog.source_profiles.map(source => {
      const button = el('button', `＋ ${source.card.name}`, 'text-button');
      button.addEventListener('click', () => {
        if (!canLeave()) return;
        selectCharacter({...defaults(), card: clone(source.card), appearance: clone(source.appearance)}); changed();
        toast('Načtená karta z projektu. Vzhled můžeš upravit ve vlastním návrhu.');
      });
      return button;
    }));
    if (characters.length) selectCharacter(characters[0]);
    else if (catalog.source_profiles.length) {
      const source = catalog.source_profiles[0];
      selectCharacter({...defaults(), card: clone(source.card), appearance: clone(source.appearance)});
    } else { normalizeAppearance(); renderForm(); renderLibrary(); }
    try {
      const module = await import('/static/model-preview.js');
      preview = await module.createCharacterPreview($('#model-preview'), {onStatus: setModelStatus});
      previewKey = ''; renderSummary();
    } catch {
      setModelStatus({state: 'unavailable', message: '3D náhled není připravený.', detail: 'Vzhled lze upravit a exportovat. Herní data náhledu zatím nejsou dostupná.'});
    }
  } catch (error) {
    $('#global-error').textContent = error.message; $('#global-error').hidden = false;
    renderForm(); renderLibrary(); $('#save-character').disabled = true;
  }
}

init();
