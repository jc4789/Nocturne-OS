/* Read-only observation of public WHATWG demo output. No DOM/style/worker changes. */
(() => {
    let last='',remaining=40;
    const timer=setInterval(() => {
        const logs=[];
        const collect=d => {
            const log=d.getElementById('log');
            if(log)logs.push(log.textContent);
            const result=d.getElementById('result');
            if(result)logs.push(result.textContent);
            for(const frame of d.querySelectorAll('iframe')) {
                try {if(frame.contentDocument)collect(frame.contentDocument);}catch(_){}
            }
        };
        collect(document);
        const text=JSON.stringify(logs);
        if(text!==last){console.log('WHATWG_WORKER_OUTPUT',location.href,text);last=text;}
        if(!--remaining)clearInterval(timer);
    },1000);
})();
