/* Read-only real-site observations. Input is sent through the guest PS/2 path. */
(() => {
  let candidate = null, announced = false, confirmed = false, focused = false, last = '';
  const focusPage = () => {
    if (focused) return;
    for (const [x, y] of [[20, 400], [innerWidth - 25, 400], [20, 200],
                        [20, innerHeight - 25], [innerWidth - 25, innerHeight - 25]]) {
      const n = document.elementFromPoint(x, y);
      if (!n || n.closest('a,button,input,textarea,select,[role="button"]')) continue;
      focused = true; candidate = {node:n, x, y, id:0}; confirmed = false;
      console.log('N8_CLICK 0 ' + x + ' ' + y);
      return;
    }
  };
  const report = kind => {
    const data = {kind, url: location.origin + location.pathname, title: document.title,
      ready: document.readyState, x: scrollX, y: scrollY,
      width: innerWidth, height: innerHeight,
      text: document.body ? document.body.innerText.slice(0, 180) : ''};
    const line = JSON.stringify(data);
    if (line !== last) console.log('N8_SITE ' + (last = line));
  };
  const pick = () => {
    if (announced) return;
    const nodes = document.querySelectorAll('a[href]');
    for (const top of [80, 0]) for (const n of nodes) {
      const r = n.getBoundingClientRect(), label = (n.textContent || '').trim();
      if (n.target && !['_self', '_top'].includes(n.target.toLowerCase())) continue;
      let destination;
      try { destination = new URL(n.href); } catch (_) { continue; }
      if (!['http:', 'https:'].includes(destination.protocol) ||
          destination.origin !== location.origin || destination.pathname === location.pathname ||
          /login|signin|account|cart|submit|ログイン/i.test(destination.pathname + ' ' + label)) continue;
      if (label.length < 4 || r.width < 8 || r.height < 8 ||
          r.left < 0 || r.top < top || r.right > innerWidth ||
          r.bottom > Math.min(innerHeight, 600)) continue;
      const x = Math.round(r.left + r.width / 2), y = Math.round(r.top + r.height / 2);
      const hit = document.elementFromPoint(x, y);
      if (!hit || !(hit === n || n.contains(hit))) continue;
      candidate = {node:n, x, y, id:1}; announced = true; confirmed = false;
      console.log('N8_LINK ' + JSON.stringify({label, href:destination.origin + destination.pathname, x, y}));
      console.log('N8_CLICK 1 ' + x + ' ' + y);
      return;
    }
  };
  addEventListener('mousemove', e => {
    if (!candidate || confirmed || !e.isTrusted) return;
    if (Math.abs(e.clientX - candidate.x) > 1 || Math.abs(e.clientY - candidate.y) > 1) return;
    if (!(e.target === candidate.node || candidate.node.contains(e.target))) return;
    const hit = document.elementFromPoint(candidate.x, candidate.y);
    if (!hit || !(hit === candidate.node || candidate.node.contains(hit))) return;
    confirmed = true;
    console.log('N8_TARGET ' + candidate.id + ' ' + candidate.x + ' ' + candidate.y);
  }, true);
  addEventListener('click', e => {
    if (e.isTrusted) console.log('N8_TRUSTED_CLICK ' + (e.target && e.target.nodeName));
  }, true);
  addEventListener('scroll', () => report('scroll'));
  report('start');
  let ticks = 0;
  const timer = setInterval(() => {
    report('tick');
    if (ticks === 0) focusPage();
    if (++ticks >= 15) pick();
    if (ticks >= 30) clearInterval(timer);
  }, 5000);
})();
