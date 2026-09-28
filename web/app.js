// Shared helpers for all OpenKiln pages.
const $ = id => document.getElementById(id);

async function req(url, opt) {
  const r = await fetch(url, Object.assign({cache: 'no-store'}, opt || {}));
  const t = await r.text();
  if (!r.ok) throw Error(t || ('HTTP ' + r.status));
  try { return JSON.parse(t); } catch (e) { return t; }
}

function post(url, data) {
  const body = new URLSearchParams();
  for (const k in data || {}) body.set(k, data[k]);
  return req(url, {method: 'POST', body});
}

function esc(v) {
  return String(v == null ? '' : v).replace(/[&<>"']/g, m => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[m]));
}

function fmt(v, d, unit) {
  return v == null || !isFinite(v) ? '—' : Number(v).toFixed(d) + (unit || '');
}

// Draws a firing profile. `marker` = {minute, temp} draws the kiln's current position.
function drawProfile(canvas, points, marker) {
  const r = canvas.getBoundingClientRect(), q = window.devicePixelRatio || 1;
  const W = Math.max(300, r.width), H = 300;
  canvas.width = W * q; canvas.height = H * q;
  const x = canvas.getContext('2d');
  x.setTransform(q, 0, 0, q, 0, 0);
  x.clearRect(0, 0, W, H);
  x.font = '12px system-ui';
  const pad = {l: 55, r: 20, t: 20, b: 38};
  if (!points || points.length < 2) { x.fillStyle = '#64748b'; x.fillText('No profile data', 20, 30); return; }
  const maxM = Math.max(1, ...points.map(p => p[0]));
  const maxT = Math.ceil(Math.max(100, ...points.map(p => p[1]), marker && isFinite(marker.temp) ? marker.temp : 0) / 100) * 100;
  const px = m => pad.l + m / maxM * (W - pad.l - pad.r);
  const py = t => H - pad.b - t / maxT * (H - pad.t - pad.b);
  for (let i = 0; i <= 5; i++) {
    const yy = pad.t + (H - pad.t - pad.b) * i / 5;
    x.strokeStyle = '#e5e7eb'; x.beginPath(); x.moveTo(pad.l, yy); x.lineTo(W - pad.r, yy); x.stroke();
    x.fillStyle = '#64748b'; x.fillText(Math.round(maxT * (1 - i / 5)) + '°C', 5, yy + 4);
  }
  for (let i = 0; i <= 6; i++) {
    const xx = pad.l + (W - pad.l - pad.r) * i / 6;
    x.strokeStyle = '#eef2f7'; x.beginPath(); x.moveTo(xx, pad.t); x.lineTo(xx, H - pad.b); x.stroke();
    x.fillStyle = '#64748b'; x.fillText(Math.round(maxM * i / 6) + 'm', xx - 8, H - 12);
  }
  x.strokeStyle = '#1769ff'; x.lineWidth = 3; x.beginPath();
  points.forEach((p, i) => i ? x.lineTo(px(p[0]), py(p[1])) : x.moveTo(px(p[0]), py(p[1])));
  x.stroke();
  if (marker && isFinite(marker.minute) && isFinite(marker.temp)) {
    const mx = px(Math.min(marker.minute, maxM)), my = py(marker.temp);
    x.strokeStyle = '#ff7a0080'; x.lineWidth = 1; x.beginPath(); x.moveTo(mx, pad.t); x.lineTo(mx, H - pad.b); x.stroke();
    x.fillStyle = '#ff7a00'; x.beginPath(); x.arc(mx, my, 6, 0, 7); x.fill();
  }
}

// Header: highlight the current page and keep the Wi-Fi indicator fresh.
(function () {
  document.querySelectorAll('.nav a').forEach(a => { if (a.getAttribute('href') === location.pathname) a.classList.add('on'); });
  const w = $('wifi');
  if (!w) return;
  async function update() {
    try {
      const d = await req('/api/wifi-status');
      w.classList.toggle('off', !d.connected);
      $('wifiTip').innerHTML = (d.connected
        ? '<b>Wi‑Fi connected</b><br>SSID: ' + esc(d.ssid) + '<br>IP: ' + esc(d.ip) + '<br>Signal: ' + d.rssi + ' dBm'
        : 'Wi‑Fi disconnected') + (d.ap_enabled ? '<br>AP: ' + esc(d.ap_ssid) + ' (' + esc(d.ap_ip) + ')' : '');
      document.dispatchEvent(new CustomEvent('wifi', {detail: d}));
    } catch (e) {}
  }
  update();
  setInterval(update, 10000);
})();
