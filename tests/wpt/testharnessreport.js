/* Nocturne's testharnessreport.js. scripts/wpt.py serves it at /resources/testharnessreport.js in
   place of the WPT one: a top-level test document sends its results to the harness and goes to the
   test the harness names next. The pieces it uses are taken before the test's own script runs, so
   a test that replaces XMLHttpRequest or JSON still reports. */
(function () {
  if (typeof window === "undefined" || window !== window.top) return;
  var url = String(location.href);
  var XHR = window.XMLHttpRequest;
  var open = XHR.prototype.open, send = XHR.prototype.send, setHeader = XHR.prototype.setRequestHeader;
  var stringify = JSON.stringify, call = Function.prototype.call;
  var replace = location.replace; /* [LegacyUnforgeable]: a test cannot replace it */

  function text(value) {
    return value === undefined || value === null ? null : String(value);
  }

  setup({output: false, timeout_multiplier: %(timeout_multiplier)s, explicit_timeout: false});
  add_completion_callback(function (tests, status) {
    var list = [];
    for (var i = 0; i < tests.length; i++) {
      var t = tests[i];
      list.push({name: text(t.name), status: t.status, message: text(t.message)});
    }
    var body = call.call(stringify, JSON, {url: url, status: status.status, message: text(status.message),
                                           stack: text(status.stack), tests: list});
    var xhr = new XHR();
    xhr.onload = function () {
      if (xhr.responseText) call.call(replace, location, xhr.responseText);
    };
    call.call(open, xhr, "POST", "/_nocturne/harness.py");
    call.call(setHeader, xhr, "Content-Type", "text/plain;charset=utf-8");
    call.call(send, xhr, body);
  });
})();
