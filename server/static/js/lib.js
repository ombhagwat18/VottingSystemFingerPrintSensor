// React + htm glue. `html` is JSX-like tagged templates, so no build step is needed.
export const { useState, useEffect, useRef, useMemo, useCallback } = React;

function h(type, props, ...children) {
  if (props) {
    if ('class' in props) { props.className = props.class; delete props.class; }
    if ('for' in props) { props.htmlFor = props.for; delete props.for; }
  }
  return React.createElement(type, props, ...children);
}
export const html = htm.bind(h);
export const mount = (el, node) => ReactDOM.createRoot(el).render(node);
