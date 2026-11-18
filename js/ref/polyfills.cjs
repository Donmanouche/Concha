// Node 20 lacks Promise.withResolvers (Node 22), used by node-web-audio-api's AudioWorklet.
if (!Promise.withResolvers)
  Promise.withResolvers = () => {
    let resolve, reject;
    const promise = new Promise((a, b) => ((resolve = a), (reject = b)));
    return { promise, resolve, reject };
  };
