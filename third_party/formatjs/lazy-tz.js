/* Nocturne adapter for FormatJS 7.8.1: retain every official packed zone, but
 * let the unchanged upstream decoder expand a zone only on first use.
 * No timezone is substituted, truncated, or removed. */
const constructor = Intl.DateTimeFormat;
const add = constructor.__addTZData;
function lazy(data) {
    const zones = Object.create(null);
    for (const packed of data.zones) {
        const name = packed.slice(0, packed.indexOf('|'));
        Object.defineProperty(zones, name, {enumerable:true, configurable:true, get() {
            const previous = constructor.tzData;
            try {
                add({abbrvs:data.abbrvs, offsets:data.offsets, zones:[packed]});
                const decoded = constructor.tzData[name];
                Object.defineProperty(zones, name, {value:decoded, enumerable:true, configurable:true, writable:true});
                return decoded;
            } finally { constructor.tzData = previous; }
        }});
    }
    constructor.tzData = zones;
}
constructor.__addTZData = lazy;
export function restore() { constructor.__addTZData = add; }
