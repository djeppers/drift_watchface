var STORAGE_BG  = 'bg_color';
var STORAGE_SEC = 'sec_interval';
var STORAGE_BAT = 'bat_threshold';

Pebble.addEventListener('showConfiguration', function () {
  var curBg  = localStorage.getItem(STORAGE_BG)  || '0';
  var curSec = localStorage.getItem(STORAGE_SEC) || '0';
  var curBat = localStorage.getItem(STORAGE_BAT) || '1';

  var watchInfo = Pebble.getActiveWatchInfo();
  var isColor = ['basalt', 'chalk', 'emery', 'gabbro'].indexOf(watchInfo.platform) !== -1;
  var bgOptions = isColor ? [
    { value: '0', label: 'White'         },
    { value: '1', label: 'Light Gray'    },
    { value: '2', label: 'Pastel Yellow' }
  ] : [
    { value: '0', label: 'Default'       },
    { value: '1', label: 'Light'         },
    { value: '2', label: 'Warm'          }
  ];

  var secOptions = [
    { value: '0', label: 'Smooth (100ms)'      },
    { value: '1', label: 'Half-second (500ms)' },
    { value: '2', label: 'Ticking (1s)'        }
  ];

  var batOptions = [
    { value: '0', label: 'Below 5%'  },
    { value: '1', label: 'Below 10%' },
    { value: '2', label: 'Below 25%' },
    { value: '3', label: 'Off'        }
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
