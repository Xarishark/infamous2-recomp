// Paste into a YouTube watch page (browser devtools console) AFTER any
// pre-roll ad has finished (check: document.querySelector('#movie_player').classList.contains('ad-showing') === false).
// Reads decoded video frames into a canvas (YouTube MSE frames are not cross-origin tainted) and
// measures regions in 1280x720 frame coordinates. Nothing is downloaded or saved.
// Usage:  await grab(219); stats(640,544,1120,704)  ->  {mean:[r,g,b], l50, l95, lmax, hiPct, clipPct}
//         await bandProfile(158, 100,400, 0,400)   ->  row-to-row step sizes at native video resolution
const p = document.querySelector('#movie_player');
try { const lv = p.getAvailableQualityLevels(); p.setPlaybackQualityRange(lv[0], lv[0]); } catch (e) {}
window.full = () => { const v = document.querySelector('video'); if (v.parentElement !== document.body) document.body.appendChild(v);
  v.style.cssText = 'position:fixed!important;left:0!important;top:0!important;width:100vw!important;height:100vh!important;object-fit:contain!important;z-index:2147483647!important;background:#000!important;transform:none!important'; };
window.seekTo = async (t) => { full(); const v = document.querySelector('video'); v.pause();
  await Promise.race([new Promise(r => { v.addEventListener('seeked', r, { once: true }); v.currentTime = t; }), new Promise(r => setTimeout(r, 8000))]);
  await new Promise(r => setTimeout(r, 1500)); full(); return v.currentTime; };
window.C = document.createElement('canvas'); C.width = 1280; C.height = 720; window.G = C.getContext('2d', { willReadFrequently: true });
window.grab = async (t) => { await seekTo(t); const v = document.querySelector('video'); G.drawImage(v, 0, 0, 1280, 720); window.D = G.getImageData(0, 0, 1280, 720).data;
  return [v.currentTime, v.videoWidth, Math.round(v.duration), p.classList.contains('ad-showing') ? 'AD' : 'ok']; };
window.stats = (x0, y0, x1, y1) => { let n = 0, sr = 0, sg = 0, sb = 0, clip = 0, hi = 0; const L = [];
  for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) { const i = (y * 1280 + x) * 4; const r = D[i], g = D[i + 1], b = D[i + 2];
    sr += r; sg += g; sb += b; n++; const l = 0.2126 * r + 0.7152 * g + 0.0722 * b; L.push(l); if (r >= 250 && g >= 250 && b >= 250) clip++; if (l >= 235) hi++; }
  L.sort((a, b) => a - b); const q = f => Math.round(L[Math.floor(f * (L.length - 1))]);
  return { mean: [Math.round(sr / n), Math.round(sg / n), Math.round(sb / n)], l50: q(0.5), l95: q(0.95), lmax: q(1), hiPct: +(100 * hi / n).toFixed(2), clipPct: +(100 * clip / n).toFixed(2) }; };
window.lum = (x, y) => { const i = (y * 1280 + x) * 4; return Math.round(0.2126 * D[i] + 0.7152 * D[i + 1] + 0.0722 * D[i + 2]); };
window.radial = (cx, cy) => [0, 2, 4, 6, 8, 12, 16, 20, 24, 32, 40, 48, 64].map(r => { let s = 0, n = 0;
  for (let a = 0; a < 32; a++) { const x = Math.round(cx + r * Math.cos(a * Math.PI / 16)), y = Math.round(cy + r * Math.sin(a * Math.PI / 16));
    if (x >= 0 && x < 1280 && y >= 0 && y < 720) { s += lum(x, y); n++; } } return r + ':' + Math.round(s / n); }).join(' ');
window.bandProfile = async (t, x0, x1, y0, y1) => { await seekTo(t); const v = document.querySelector('video'); const W = v.videoWidth, H = v.videoHeight;
  const c = document.createElement('canvas'); c.width = W; c.height = H; const g = c.getContext('2d', { willReadFrequently: true }); g.drawImage(v, 0, 0);
  const d = g.getImageData(x0, y0, x1 - x0, y1 - y0).data; const w = x1 - x0; const rows = [];
  for (let y = 0; y < y1 - y0; y++) { const s = [0, 0, 0]; for (let x = 0; x < w; x++) { const i = (y * w + x) * 4; s[0] += d[i]; s[1] += d[i + 1]; s[2] += d[i + 2]; } rows.push(s.map(q => q / w)); }
  const steps = []; for (let i = 1; i < rows.length; i++) steps.push(Math.max(...[0, 1, 2].map(k => Math.abs(rows[i][k] - rows[i - 1][k])))); steps.sort((a, b) => a - b);
  return { W, H, medianStep: steps[Math.floor(steps.length / 2)], p95Step: steps[Math.floor(steps.length * 0.95)], maxStep: steps[steps.length - 1] }; };
