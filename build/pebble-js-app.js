/******/ (function(modules) { // webpackBootstrap
/******/ 	// The module cache
/******/ 	var installedModules = {};
/******/
/******/ 	// The require function
/******/ 	function __webpack_require__(moduleId) {
/******/
/******/ 		// Check if module is in cache
/******/ 		if(installedModules[moduleId])
/******/ 			return installedModules[moduleId].exports;
/******/
/******/ 		// Create a new module (and put it into the cache)
/******/ 		var module = installedModules[moduleId] = {
/******/ 			exports: {},
/******/ 			id: moduleId,
/******/ 			loaded: false
/******/ 		};
/******/
/******/ 		// Execute the module function
/******/ 		modules[moduleId].call(module.exports, module, module.exports, __webpack_require__);
/******/
/******/ 		// Flag the module as loaded
/******/ 		module.loaded = true;
/******/
/******/ 		// Return the exports of the module
/******/ 		return module.exports;
/******/ 	}
/******/
/******/
/******/ 	// expose the modules object (__webpack_modules__)
/******/ 	__webpack_require__.m = modules;
/******/
/******/ 	// expose the module cache
/******/ 	__webpack_require__.c = installedModules;
/******/
/******/ 	// __webpack_public_path__
/******/ 	__webpack_require__.p = "";
/******/
/******/ 	// Load entry module and return exports
/******/ 	return __webpack_require__(0);
/******/ })
/************************************************************************/
/******/ ([
/* 0 */
/***/ (function(module, exports, __webpack_require__) {

	__webpack_require__(1);
	module.exports = __webpack_require__(2);


/***/ }),
/* 1 */
/***/ (function(module, exports) {

	/**
	 * Copyright 2024 Google LLC
	 *
	 * Licensed under the Apache License, Version 2.0 (the "License");
	 * you may not use this file except in compliance with the License.
	 * You may obtain a copy of the License at
	 *
	 *     http://www.apache.org/licenses/LICENSE-2.0
	 *
	 * Unless required by applicable law or agreed to in writing, software
	 * distributed under the License is distributed on an "AS IS" BASIS,
	 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	 * See the License for the specific language governing permissions and
	 * limitations under the License.
	 */
	
	(function(p) {
	  if (!p === undefined) {
	    console.error('Pebble object not found!?');
	    return;
	  }
	
	  // Aliases:
	  p.on = p.addEventListener;
	  p.off = p.removeEventListener;
	
	  // For Android (WebView-based) pkjs, print stacktrace for uncaught errors:
	  if (typeof window !== 'undefined' && window.addEventListener) {
	    window.addEventListener('error', function(event) {
	      if (event.error && event.error.stack) {
	        console.error('' + event.error + '\n' + event.error.stack);
	      }
	    });
	  }
	
	})(Pebble);


/***/ }),
/* 2 */
/***/ (function(module, exports) {

	var STORAGE_BG  = 'bg_color';
	var STORAGE_SEC = 'sec_interval';
	var STORAGE_BAT = 'bat_threshold';
	
	Pebble.addEventListener('showConfiguration', function () {
	  var curBg  = localStorage.getItem(STORAGE_BG)  || '0';
	  var curSec = localStorage.getItem(STORAGE_SEC) || '0';
	  var curBat = localStorage.getItem(STORAGE_BAT) || '1';
	
	  var bgOptions = [
	    { value: '0', label: 'White'         },
	    { value: '1', label: 'Light Gray'    },
	    { value: '2', label: 'Pastel Yellow' }
	  ];
	
	  var secOptions = [
	    { value: '0', label: 'Smooth (100ms)'      },
	    { value: '1', label: 'Half-second (500ms)' },
	    { value: '2', label: 'Ticking (1s)'        }
	  ];
	
	  var batOptions = [
	    { value: '0', label: 'Below 5%'  },
	    { value: '1', label: 'Below 10%' },
	    { value: '2', label: 'Below 25%' }
	  ];
	
	  function buildSelect(id, options, current) {
	    return '<select id="' + id + '">' +
	      options.map(function (o) {
	        return '<option value="' + o.value + '"' +
	          (o.value === current ? ' selected' : '') +
	          '>' + o.label + '</option>';
	      }).join('') +
	    '</select>';
	  }
	
	  var html = [
	    '<!DOCTYPE html><html>',
	    '<head><meta name="viewport" content="width=device-width,initial-scale=1">',
	    '<style>',
	    '  body   { font-family: sans-serif; padding: 24px; background: #f5f5f5; }',
	    '  h2     { margin-bottom: 16px; }',
	    '  label  { display: block; margin: 16px 0 4px; font-size: 14px; color: #555; }',
	    '  select { width: 100%; padding: 10px; font-size: 16px; }',
	    '  button { width: 100%; margin-top: 24px; padding: 12px; font-size: 16px;',
	    '           background: #333; color: #fff; border: none; border-radius: 4px; }',
	    '</style></head>',
	    '<body>',
	    '<h2>Drift</h2>',
	    '<label>Background Color</label>',
	    buildSelect('bg', bgOptions, curBg),
	    '<label>Seconds Hand</label>',
	    buildSelect('sec', secOptions, curSec),
	    '<label>Battery Alert</label>',
	    buildSelect('bat', batOptions, curBat),
	    '<button onclick="save()">Save</button>',
	    '<script>',
	    'function save() {',
	    '  var bg  = document.getElementById("bg").value;',
	    '  var sec = document.getElementById("sec").value;',
	    '  var bat = document.getElementById("bat").value;',
	    '  location.href = "pebblejs://close#" + encodeURIComponent(JSON.stringify({',
	    '    BG_COLOR:      parseInt(bg,  10),',
	    '    SEC_INTERVAL:  parseInt(sec, 10),',
	    '    BAT_THRESHOLD: parseInt(bat, 10)',
	    '  }));',
	    '}',
	    '</script>',
	    '</body></html>'
	  ].join('');
	
	  Pebble.openURL('data:text/html,' + encodeURIComponent(html));
	});
	
	Pebble.addEventListener('webviewclosed', function (e) {
	  if (!e.response) return;
	
	  var config;
	  try {
	    config = JSON.parse(decodeURIComponent(e.response));
	  } catch (err) {
	    return;
	  }
	
	  localStorage.setItem(STORAGE_BG,  config.BG_COLOR.toString());
	  localStorage.setItem(STORAGE_SEC, config.SEC_INTERVAL.toString());
	  localStorage.setItem(STORAGE_BAT, config.BAT_THRESHOLD.toString());
	
	  Pebble.sendAppMessage(
	    { BG_COLOR: config.BG_COLOR, SEC_INTERVAL: config.SEC_INTERVAL,
	      BAT_THRESHOLD: config.BAT_THRESHOLD },
	    function () { console.log('config sent'); },
	    function () { console.log('sendAppMessage failed'); }
	  );
	});


/***/ })
/******/ ]);
//# sourceMappingURL=pebble-js-app.js.map