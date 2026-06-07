(function () {
  "use strict";

  var cTypes = [
    "bool",
    "char",
    "double",
    "float",
    "int8_t",
    "int16_t",
    "int32_t",
    "int64_t",
    "intptr_t",
    "size_t",
    "uint8_t",
    "uint16_t",
    "uint32_t",
    "uint64_t",
    "uintptr_t"
  ];

  var typePattern = cTypes
    .slice()
    .sort(function (a, b) { return b.length - a.length; })
    .join("|");

  var declarationPattern = new RegExp("\\b(" + typePattern + ")\\b(\\s*(?:\\*\\s*)?)([A-Za-z_]\\w*)(?=\\s*(?:[,;)\\]=]|$))", "g");
  var plainTypePattern = new RegExp("\\b(" + typePattern + ")\\b", "g");

  function escapeHtml(text) {
    return text
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;");
  }

  function highlightText(text) {
    var escaped = escapeHtml(text);

    escaped = escaped.replace(declarationPattern, function (_match, typeName, spacing, varName) {
      return '<span class="c-plain-type">' + typeName + '</span>' +
        spacing +
        '<span class="c-plain-var">' + varName + '</span>';
    });

    return escaped.replace(plainTypePattern, function (match, typeName, offset, fullText) {
      var before = fullText.slice(Math.max(0, offset - 28), offset);
      if (before.indexOf('class="c-plain-type">') !== -1) {
        return match;
      }
      return '<span class="c-plain-type">' + typeName + '</span>';
    });
  }

  function shouldSkip(node) {
    var parent = node.parentElement;
    return !parent ||
      parent.closest("a, span, script, style") !== null ||
      parent.classList.contains("lineno");
  }

  function replaceTextNode(node) {
    if (shouldSkip(node) || !/\b(?:u?int|size_t|bool|char|float|double)/.test(node.nodeValue)) {
      return;
    }

    var html = highlightText(node.nodeValue);
    if (html === escapeHtml(node.nodeValue)) {
      return;
    }

    var wrapper = document.createElement("span");
    wrapper.innerHTML = html;
    node.parentNode.replaceChild(wrapper, node);
  }

  function enhanceCodeBlocks() {
    var lines = document.querySelectorAll(".fragment div.line");
    Array.prototype.forEach.call(lines, function (line) {
      var walker = document.createTreeWalker(line, NodeFilter.SHOW_TEXT);
      var nodes = [];
      var current;

      while ((current = walker.nextNode())) {
        nodes.push(current);
      }

      nodes.forEach(replaceTextNode);
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", enhanceCodeBlocks);
  } else {
    enhanceCodeBlocks();
  }
}());
