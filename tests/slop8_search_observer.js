/* Read-only search observation; characters and Enter use real PS/2 input. */
(() => {
  let input = null, confirmed = false, announced = false, ticks = 0, x = 0, y = 0;
  const report = () => {
    const u = new URL(location.href);
    console.log('N8_SEARCH ' + JSON.stringify({url:u.origin + u.pathname,
      query:u.searchParams.get('k') || u.searchParams.get('q'),
      title:document.title, y:scrollY, ready:document.readyState}));
  };
  addEventListener('mousemove', e => {
    if (!input || confirmed || !e.isTrusted || e.target !== input ||
        Math.abs(e.clientX - x) > 1 || Math.abs(e.clientY - y) > 1 ||
        document.elementFromPoint(x, y) !== input) return;
    confirmed = true;
    console.log('N8_TARGET 1 ' + x + ' ' + y);
  }, true);
  addEventListener('click', e => {
    if (e.isTrusted && e.target === input) console.log('N8_SEARCH_FOCUSED');
  }, true);
  addEventListener('input', e => {
    if (e.isTrusted && e.target === input) console.log('N8_SEARCH_INPUT ' + input.value);
  }, true);
  report();
  setInterval(() => {
    report();
    if (++ticks < 3 || announced) return;
    const n = document.querySelector('input[type="search"],input[name="field-keywords"],input[name="q"]');
    if (!n || n.disabled || n.readOnly) return;
    const r = n.getBoundingClientRect();
    if (r.width < 20 || r.height < 10 || r.left < 0 || r.top < 0 || r.bottom > innerHeight) return;
    x = Math.round(r.left + r.width / 2); y = Math.round(r.top + r.height / 2);
    if (document.elementFromPoint(x, y) !== n) return;
    input = n; announced = true;
    console.log('N8_CLICK 1 ' + x + ' ' + y);
  }, 5000);
})();
