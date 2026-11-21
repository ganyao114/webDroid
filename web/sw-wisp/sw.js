/* Register within the WebView's same-origin scope. */
self.addEventListener('install', event => event.waitUntil(self.skipWaiting()));
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', event => {
  const request = event.request;
  const url = new URL(request.url);
  if (url.origin !== self.location.origin || !url.href.startsWith(self.registration.scope)) return;
  if (request.method !== 'GET' && request.method !== 'HEAD') return;

  event.respondWith((async () => {
    const response = await fetch(request);
    if (response.type !== 'basic' && response.type !== 'default') return response;
    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    if (request.mode === 'navigate') headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    return new Response(response.body, {
      status: response.status, statusText: response.statusText, headers,
    });
  })());
});
