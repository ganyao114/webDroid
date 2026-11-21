addToLibrary({
  $waBinderBridge: null,

  clock_ns__sig: 'ji',
  clock_ns: realtime => realtime ? BigInt(Date.now()) * 1_000_000n
    : BigInt(Math.round(performance.timeOrigin * 1_000_000)) +
      BigInt(Math.round(performance.now() * 1_000_000)),

  binder_init__sig: 'iii',
  binder_init__deps: ['$waBinderBridge'],
  binder_init: (bytes, pid) => {
    if (!Module['binderHost']) throw new Error('No external Binder ring for this vproc');
    waBinderBridge = Module['binderHost'].imports(wasmMemory);
    return waBinderBridge.binder_init(bytes, pid);
  },

  binder_send__sig: 'iiiiiii',
  binder_send__deps: ['$waBinderBridge'],
  binder_send: (dst, txn, code, flags, pointer, length) =>
    waBinderBridge.binder_send(dst, txn, code, flags, pointer, length),

  binder_recv__sig: 'iiiii',
  binder_recv__deps: ['$waBinderBridge'],
  binder_recv: (header, payload, maxPayload, timeout) =>
    waBinderBridge.binder_recv(header, payload, maxPayload, timeout),
});
