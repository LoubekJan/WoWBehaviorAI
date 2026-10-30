/* Native 3.3.5a geometry, geosets and texture layers, from the owned client. */
const ROOT = '/model-data/';
export function createCharacterPreview(container, {onStatus = () => {}} = {}) {
  let dead = false, sequence = 0, renderer, scene, camera, group, THREE, manifest, observer, frame;
  let angle = 0, distance = 4.6, dragging = false, lastX = 0, resources = [];
  const images = new Map(), meshes = new Map(), abort = new AbortController();
  const status = (state, message, detail = '') => { if (!dead) onStatus({state, message, detail}); };
  const json = async path => {
    if (!/^[A-Za-z0-9_/.\-]+$/.test(path) || path.includes('..')) throw Error('Invalid path');
    const version = manifest?.asset_revision;
    const suffix = path !== 'manifest.json' && /^[a-f0-9]{64}$/.test(version || '') ? `?v=${version}` : '';
    const response = await fetch(ROOT + path + suffix, {signal: abort.signal, cache: path === 'manifest.json' ? 'no-store' : 'no-cache'});
    if (!response.ok) throw Error('Unavailable model data');
    return response.json();
  };
  const image = path => {
    if (!path) return Promise.resolve(null);
    if (!images.has(path)) images.set(path, new Promise((resolve, reject) => {
      if (!/^textures\/[a-f0-9]+\.png$/.test(path)) { reject(Error('Invalid texture')); return; }
      const item = new Image(); item.onload = () => resolve(item); item.onerror = () => reject(Error('Missing texture')); item.src = ROOT + path + (/^[a-f0-9]{64}$/.test(manifest?.asset_revision || '') ? `?v=${manifest.asset_revision}` : '');
    }));
    return images.get(path);
  };
  const release = () => { if (group) scene.remove(group); for (const item of resources) item.dispose(); resources = []; group = null; };
  const resize = () => {
    if (!renderer || dead) return;
    const width = Math.max(1, container.clientWidth), height = Math.max(1, container.clientHeight);
    renderer.setSize(width, height, false); camera.aspect = width / height; camera.updateProjectionMatrix();
  };
  const draw = () => { if (dead) return; if (group) group.rotation.y = angle; renderer.render(scene, camera); frame = requestAnimationFrame(draw); };
  const down = event => { if (event.button !== 0) return; dragging = true; lastX = event.clientX; container.setPointerCapture(event.pointerId); };
  const move = event => { if (dragging) { angle += (event.clientX - lastX) * 0.012; lastX = event.clientX; } };
  const up = () => { dragging = false; };
  const wheel = event => { if (!camera) return; event.preventDefault(); distance = Math.max(2.1, Math.min(7, distance + event.deltaY * .003)); camera.position.z = distance; };
  const ready = (async () => {
    status('loading', 'Načítám modelová data klientu…');
    manifest = await json('manifest.json');
    if (manifest.format !== 'wow335-native-static' || manifest.client_build !== 12340 || !manifest.variants) throw Error('Unsupported model format');
    THREE = await import('./vendor/three.module.min.js');
    if (dead) return false;
    renderer = new THREE.WebGLRenderer({alpha: true, antialias: true, powerPreference: 'low-power'});
    renderer.setPixelRatio(Math.min(devicePixelRatio || 1, 2)); renderer.outputColorSpace = THREE.SRGBColorSpace; renderer.setClearColor(0, 0);
    renderer.domElement.setAttribute('role', 'img'); renderer.domElement.setAttribute('aria-label', 'Původní model 3.3.5a ve statické póze. Tažením otočíš postavu.');
    container.replaceChildren(renderer.domElement);
    scene = new THREE.Scene(); camera = new THREE.PerspectiveCamera(32, 1, .05, 30); camera.position.set(0, 1.1, distance); camera.lookAt(0, 1.05, 0);
    scene.add(new THREE.HemisphereLight(0xffe3c5, 0x394a61, 2.4));
    const key = new THREE.DirectionalLight(0xffedda, 2.5); key.position.set(-3, 4, 5); scene.add(key);
    const rim = new THREE.DirectionalLight(0x8ca7cb, 1.6); rim.position.set(2, 2, -3); scene.add(rim);
    observer = new ResizeObserver(resize); observer.observe(container); resize(); frame = requestAnimationFrame(draw);
    container.addEventListener('pointerdown', down); container.addEventListener('pointermove', move); container.addEventListener('pointerup', up); container.addEventListener('pointercancel', up); container.addEventListener('wheel', wheel, {passive: false});
    return true;
  })().catch(() => { status('unavailable', '3D náhled není připravený.', 'Volby vzhledu lze uložit a exportovat. Náhled potřebuje modelová data místního klientu 3.3.5a a podporu WebGL.'); return false; });
  async function update(appearance) {
    const ticket = ++sequence, a = structuredClone(appearance);
    if (!await ready || dead || ticket !== sequence) return;
    const variant = manifest.variants[`${a.race}_${a.gender}`];
    if (!variant) { release(); status('unavailable', 'Model této rasy ještě není připravený.', 'Konfigurace vzhledu a export do hry zůstávají dostupné.'); return; }
    status('loading', 'Skládám vzhled postavy…');
    let pendingResources = [];
    try {
      if (!meshes.has(variant.mesh)) meshes.set(variant.mesh, json(variant.mesh));
      const mesh = await meshes.get(variant.mesh);
      const find = (section, style, color) => variant.sections.find(row => row.section === section && row.style === style && row.color === color)?.files || [];
      const skin = find(0, 0, a.skin), face = find(1, a.face, a.skin), facial = find(2, a.facial_hair, a.hair_color), hair = find(3, a.hair_style, a.hair_color), underwear = find(4, 0, a.skin);
      const [base, faceLo, faceHi, beardLo, beardHi, hairMain, hairLo, hairHi, underLo, underHi] = await Promise.all([image(skin[0]), image(face[0]), image(face[1]), image(facial[0]), image(facial[1]), image(hair[0]), image(hair[1]), image(hair[2]), image(underwear[0]), image(underwear[1])]);
      if (!base) throw Error('Missing base texture');
      const canvas = document.createElement('canvas'); canvas.width = base.width; canvas.height = base.height; const ctx = canvas.getContext('2d'); ctx.drawImage(base, 0, 0);
      const paint = (img, x, y, w, h) => { if (img) ctx.drawImage(img, x * canvas.width / 256, y * canvas.height / 256, w * canvas.width / 256, h * canvas.height / 256); };
      paint(faceLo, 0, 192, 128, 64); paint(faceHi, 0, 160, 128, 32); paint(underLo, 128, 96, 128, 64); paint(underHi, 128, 0, 128, 64);
      paint(beardLo, 0, 192, 128, 64); paint(beardHi, 0, 160, 128, 32); paint(hairLo, 0, 192, 128, 64); paint(hairHi, 0, 160, 128, 32);
      const surfaceSlots = manifest.equipment_surface_slots || [];
      const selectedGear = Array.isArray(a.equipment) ? a.equipment : [];
      const layerOrder = ['shirt', 'legs', 'feet', 'chest', 'waist', 'tabard', 'wrist', 'hands'];
      const atlas = [[0, 0, 128, 64], [0, 64, 128, 64], [0, 128, 128, 32], [128, 0, 128, 64], [128, 64, 128, 32], [128, 96, 128, 64], [128, 160, 128, 64], [128, 224, 128, 32]];
      const shownSlots = new Set();
      for (const slot of layerOrder) {
        const item = selectedGear.find(item => item.slot === slot);
        const layerFiles = item && surfaceSlots.includes(slot) ? manifest.equipment?.[String(item.display_id)]?.layers?.[String(a.gender)] : null;
        if (!layerFiles) continue;
        const layers = await Promise.all(layerFiles.map(image));
        layers.forEach((layer, index) => paint(layer, ...atlas[index]));
        if (layers.some(Boolean)) shownSlots.add(slot);
      }
      const allocated = pendingResources;
      const tex = picture => { if (!picture) return null; const t = new THREE.CanvasTexture(picture); t.colorSpace = THREE.SRGBColorSpace; t.flipY = false; allocated.push(t); return t; };
      const bodyTex = tex(canvas), hairTex = tex(hairMain), furTex = tex(await image(skin[1]));
      const materials = await Promise.all(mesh.textures.map(async item => {
        const map = item.kind === 1 ? bodyTex : item.kind === 6 ? hairTex : item.kind === 8 ? furTex : item.kind === 0 ? tex(await image(item.file)) : bodyTex;
        const material = new THREE.MeshStandardMaterial({color: map ? 0xffffff : 0x6b5542, map, roughness: 1, metalness: 0, side: THREE.DoubleSide, alphaTest: .35}); allocated.push(material); return material;
      }));
      if (dead || ticket !== sequence) { allocated.forEach(item => item.dispose()); return; }
      const next = new THREE.Group(), positions = new THREE.Float32BufferAttribute(mesh.positions, 3), normals = new THREE.Float32BufferAttribute(mesh.normals, 3), uvs = new THREE.Float32BufferAttribute(mesh.uvs, 2);
      const hairGeo = variant.hair_geosets[String(a.hair_style)]?.geoset ?? 0, facialGeo = variant.facial_geosets[String(a.facial_hair)] || [1, 1, 1];
      const gearGeosets = {};
      for (const slot of layerOrder) {
        const item = selectedGear.find(row => row.slot === slot);
        const flags = item && manifest.equipment?.[String(item.display_id)]?.geoset_groups;
        if (!flags) continue;
        if (slot === 'shirt' || slot === 'chest') { gearGeosets[8] = flags[0] + 1; if (flags[2]) gearGeosets[13] = flags[2] + 1; }
        if (slot === 'legs') { gearGeosets[9] = flags[1] + 1; if (flags[2]) gearGeosets[13] = flags[2] + 1; }
        if (slot === 'feet') gearGeosets[5] = flags[0] + 1;
        if (slot === 'hands') gearGeosets[4] = flags[0] + 1;
      }
      if ((gearGeosets[13] || 1) > 1) gearGeosets[5] = 1;
      const visible = id => { if (!id) return true; if (id < 100) return id === hairGeo; const category = Math.floor(id / 100); if (category >= 1 && category <= 3) return id === category * 100 + (facialGeo[category - 1] || 1); return id === category * 100 + (gearGeosets[category] || 1); };
      for (const part of mesh.parts) {
        if (!visible(part.geoset)) continue;
        const geometry = new THREE.BufferGeometry(); geometry.setAttribute('position', positions); geometry.setAttribute('normal', normals); geometry.setAttribute('uv', uvs); geometry.setIndex(part.indices); allocated.push(geometry);
        next.add(new THREE.Mesh(geometry, materials[part.material] || materials[0]));
      }
      const bounds = new THREE.Box3().setFromObject(next), height = bounds.max.y - bounds.min.y;
      if (!Number.isFinite(height) || height <= 0) throw Error('Invalid model bounds');
      next.scale.setScalar(2.05 / height); next.position.y = -bounds.min.y * 2.05 / height;
      release(); group = next; resources = allocated; pendingResources = []; scene.add(group);
      const incomplete = [[0, 0, a.skin], [1, a.face, a.skin], [2, a.facial_hair, a.hair_color], [3, a.hair_style, a.hair_color], [4, 0, a.skin]].some(([section, style, color]) => variant.sections.find(row => row.section === section && row.style === style && row.color === color)?.missing_layers?.length);
      const omitted = selectedGear.length ? ` Textury oděvu: ${shownSlots.size} slotů. Objemové díly výbavy, pláště, zbraně a helmy uvidíš až ve hře.` : '';
      status('ready', 'Původní model 3.3.5a · statická póza', 'Geometrie, pleť, obličej a vlasy z místního klientu. Tažením otočíš; kolečkem přiblížíš.' + omitted + (incomplete ? ' Některé texturové vrstvy v klientu chybí; náhled je částečný.' : ''));
    } catch { pendingResources.forEach(item => item.dispose()); if (!dead && ticket === sequence) { release(); status('error', 'Tento vzhled se nepodařilo zobrazit.', 'Zkontroluj modelová data klientu. Uložená karta ani export vzhledu se tím nemění.'); } }
  }
  return { update, rotate(delta) { if (Number.isFinite(delta)) angle += delta; }, dispose() {
    dead = true; sequence++; abort.abort(); cancelAnimationFrame(frame); observer?.disconnect();
    container.removeEventListener('pointerdown', down); container.removeEventListener('pointermove', move); container.removeEventListener('pointerup', up); container.removeEventListener('pointercancel', up); container.removeEventListener('wheel', wheel);
    release(); renderer?.dispose(); renderer?.forceContextLoss(); container.replaceChildren(); images.clear(); meshes.clear();
  }};
}
