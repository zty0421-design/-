const CACHE_NAME = 'trpg-online-v56-cpp-17-affix-mobile-1';

const APP_SHELL = [
  '/',
  '/native-socket.js',
  '/manifest.webmanifest',
  '/icons/favicon-32.png',
  '/icons/icon-192.png',
  '/icons/icon-512.png',
  '/icons/apple-touch-icon.png'
];

self.addEventListener(
  'install',
  event => {
    event.waitUntil(
      caches
        .open(CACHE_NAME)
        .then(cache => cache.addAll(APP_SHELL))
        .then(() => self.skipWaiting())
    );
  }
);

self.addEventListener(
  'activate',
  event => {
    event.waitUntil(
      caches
        .keys()
        .then(
          names => Promise.all(
            names
              .filter(name => name !== CACHE_NAME)
              .map(name => caches.delete(name))
          )
        )
        .then(() => self.clients.claim())
    );
  }
);

self.addEventListener(
  'fetch',
  event => {
    if(event.request.method !== 'GET'){
      return;
    }

    const url = new URL(event.request.url);

    if(
      url.origin !== self.location.origin ||
      url.pathname.startsWith('/api/') ||
      url.pathname.startsWith('/ws')
    ){
      return;
    }

    event.respondWith(
      fetch(event.request)
        .then(
          response => {
            if(response.ok){
              const copy = response.clone();

              caches
                .open(CACHE_NAME)
                .then(cache => cache.put(event.request,copy));
            }

            return response;
          }
        )
        .catch(
          async () => {
            const cached = await caches.match(event.request);

            if(cached){
              return cached;
            }

            if(event.request.mode === 'navigate'){
              return caches.match('/');
            }

            return Response.error();
          }
        )
    );
  }
);
