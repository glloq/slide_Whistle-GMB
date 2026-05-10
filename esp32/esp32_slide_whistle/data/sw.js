// Service worker minimaliste : cache shell HTML, network-first pour API.
const CACHE = 'slidewhistle-v2';
const SHELL = ['/', '/index.html', '/app.css', '/app.js', '/manifest.webmanifest'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(SHELL)).then(() => self.skipWaiting()));
});
self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys().then(keys => Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});
self.addEventListener('fetch', e => {
  const url = new URL(e.request.url);
  // API/WS : toujours réseau, ne pas cacher
  if (url.pathname.startsWith('/api/') || url.pathname.startsWith('/ws')) return;
  // Shell : network-first avec fallback cache (utile en mode offline)
  e.respondWith(
    fetch(e.request).then(r => {
      const copy = r.clone();
      caches.open(CACHE).then(c => c.put(e.request, copy));
      return r;
    }).catch(() => caches.match(e.request))
  );
});
