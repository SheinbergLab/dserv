<!DOCTYPE html>
<html>
  <head>
    <script src="/rtl/jquery.js"></script>
    <script src="/rtl/smq.js"></script>
    <link href="https://unpkg.com/tabulator-tables@5.5.0/dist/css/tabulator.min.css" rel="stylesheet">
    <script type="text/javascript" src="https://unpkg.com/tabulator-tables@5.5.0/dist/js/tabulator.min.js"></script>

    <link rel="stylesheet" href="node_modules/xterm/css/xterm.css" />
    <script src="node_modules/xterm/lib/xterm.js"></script>
    <script src="node_modules/xterm-addon-fit/lib/xterm-addon-fit.js"></script>
    <script src="node_modules/xterm-addon-search/lib/xterm-addon-search.js"></script>
    
    <script>
      var smq = SMQ.Client(SMQ.wsURL("/QPCSBroker/"));
      
      function user_func (action) {
	  smq.publish(action, "action");
	  console.log(action);
      }
      
      var evt_types = new Array();
      for (i = 0; i < 16; i++) {  evt_types[i] = "Reserved"+i; }
      for (i = 16; i < 128; i++) {  evt_types[i] = "System"+i; }
      for (i = 128; i < 256; i++) {  evt_types[i] = "User"+i; }
      
      var evt_subtypes = new Object();

      var sysinfo;

      function processEssResult(essresult) {
	  let [cmd, code, result]  = essresult.match(/<cmd> (.*) <\/cmd> (\d) (.*)/).slice(1)
	  if (cmd == "ess::get_system_json" && code == 0) {
	      sysinfo = JSON.parse(result);
	      let sysSelect = document.getElementById('system-select');
	      let curSys = sysSelect.value
	      let options = sysSelect.getElementsByTagName('option');	      
	      for (var i = options.length; i--; ) {
		  sysSelect.removeChild(options[i]);
	      }
	      var newSys
	      for (var sys in sysinfo) {
		  if (sys == curSys) { newSys = sys; }
		  sysSelect.add(new Option(sys));
	      }
	      if (newSys) { sysSelect.value = newSys; }
	      setSystem(sysSelect)
	  }
      }

      function setSystem(obj) {
	  smq.publish("ess::load_system "+obj.value, "ess-cmd");
      }

      function refreshSystems() {
	  smq.publish("ess::get_system_json", "ess-cmd");
      }

      function initializeESS() {
	  smq.publish("package require ess", "ess-cmd");
      }
      
      smq.subscribe("ess-result", {onmsg:processEssResult, datatype:"text"});

      initializeESS();
      refreshSystems();
      
      $(function() {
	  var sname;
	  var start_time;
	  var last_time;
	  var action_only = true;
	  
	  var evt_start_time;
	  var evt_last_time;
	  
	  function addEssToTable(txt) {
	      if  (txt.match("^RESET")) {
		  ess_table.clearData();
	      }
	      else {
		  s = txt.split(' ');
		  sname = s[4].replace(/[{}]/g, "");
		  if ( sname.match("^start_t") ) {
		      start_time = Number(s[2]);
		  }
		  if ( action_only && sname.match("_t$") ) {
		      return;
		  }
		  sname = sname.replace(/_[ta]$/g,"");
		  
		  let current_time = Number(s[2])-start_time;
		  let elapsed = Math.max(Math.round((current_time-last_time)/1000) || 0, 0);
		  
		  ess_table.addData([
		      { time: Math.round(current_time/1000), state: sname, elapsed: elapsed},
		      
		  ])
		  last_time = current_time
	      }
	  };
	  
	  function base64ToArrayBuffer(base64) {
	      var binaryString = atob(base64);
	      var bytes = new Uint8Array(binaryString.length);
	      for (var i = 0; i < binaryString.length; i++) {
		  bytes[i] = binaryString.charCodeAt(i);
	      }
	      return bytes.buffer;
	  }	  
	  function addEvtToTable(txt) {
	      if  (txt.match("^RESET")) {
		  evt_table.clearData();
	      }
	      else {
		  var array;
		  s = txt.split(' ');
		  paramstr = s.slice(4).join(' ').replace(/[{}]/g, "");
		  let [e, type, subtype] = s[0].split(':')
		  let nbytes = s[3]
		  
		  if ( type == 3 ) { return; }
		  if ( type == 1 ) {
		      evt_types[subtype] = paramstr;
		      return;
		  }
		  if ( type == 6 ) {
		      var stypes = paramstr.split(' ');
		      for(var i=0; i < stypes.length; i+=2){
			  evt_subtypes[""+subtype+":"+stypes[i+1]] = stypes[i];
		      }
		      return;
		  }
		  if ( type == 19 ) {
		      evt_start_time = s[2];
		  }
		  let t = Math.round((s[2]-evt_start_time)/1000)
		  let dtype = s[1];
		  
		  //		  console.log(txt);
		  
		  if ( nbytes != 0 ) {
		      if (dtype == 1) {
			  arraystr = paramstr;
		      }
		      else {
			  bytearray = base64ToArrayBuffer(paramstr);
			  if (dtype == 0) {
			      arraystr = new Uint8Array(bytearray).reverse().toString();
			  } else if (dtype == 2) {
			      let array = new Float32Array(bytearray).reverse()
			      arraystr = array.toString();
			  } else if (dtype == 4) {
			      arraystr = new Uint16Array(bytearray).reverse().toString();
			  } else if (dtype == 5) {
			      arraystr = new Uint32Array(bytearray).reverse().toString();
			  } else {
			      return;
			  }
		      }
		  } else {
		      arraystr = "";
		  }
		  
		  var stype = evt_subtypes[""+type+":"+subtype] || "";
		  var etype = evt_types[type] || type;
		  evt_table.addData([
		      { time: t , type: etype, subtype: stype, params: arraystr },
		  ])
	      }
	  };
	  
	  function essMessage(msg) {
	      addEssToTable(msg);
	  };
	  
	  function evtMessage(msg) {
	      addEvtToTable(msg);
	  };
	  
	  smq.subscribe("ess", {onmsg:essMessage, datatype:"text"});
	  smq.subscribe("eventlog", {onmsg:evtMessage, datatype:"text"});	  
      });
      
    </script>
    
  </head>
  <body>
    <div class="wrapper">
      <div class="stimgui">
	<div class="system-selector">
	  <div>
	    <label for="system-select">System:</label>
	    <select name="System" id="system-select" onchange="setSystem(this);"></select>  
	  </div>
	  <div>
	    <label for="protocol-select">Protocol:</label>
	    <select name="Protocol" id="protocol-select"></select>  
	  </div>
	  <div>
	    <label for="variant-select">Variant:</label>
	    <select name="Variant" id="variant-select"></select>  
	  </div>

	  <div>
	  <button onclick="refreshSystems()" id="refresh_button" name="refresh">Refresh</button>
	  </div>
	  
	</div>
	<div class="btn-group">
	  
	  <button onclick="user_func('USER_START')" id="go_button" name="go">Go</button>
	  <button onclick="user_func('USER_STOP')" id="stop_button" name="stop">Stop</button>
	  <button onclick="user_func('USER_RESET')" id="reset_button" name="reset">Reset</button>
	</div>
      </div>
      <div class="term" id="terminal"></div>
      <div class="box1">
	<h2>Ess Viewer</h2>
	<div id="ess-table"></div>
      </div>
      <div class="box2">
	<h2>Event Viewer</h2>
	<div id="evt-table"></div>
      </div>
    </div>
    <script>
      //define some sample data
      var ess_tabledata = [];
      //create Tabulator on DOM element with id "ess-table"
      var ess_table = new Tabulator("#ess-table", {
	  height: 500,
	  data: ess_tabledata, //assign data to table
	  layout: "fitColumns", //fit columns to width of table (optional)
	  columns: [ //Define Table Columns
	      { title: "Time", field: "time", hozAlign: "center"  },
	      { title: "State", field: "state", hozAlign: "left", hozAlign: "center"  },
	      { title: "Elapsed", field: "elapsed", hozAlign: "center" },
	  ],
      });
      
      //define some sample data
      var evt_tabledata = [];
      //create Tabulator on DOM element with id "evt-table"
      var evt_table = new Tabulator("#evt-table", {
	  height: 500,
	  data: evt_tabledata, //assign data to table
	  layout: "fitColumns", //fit columns to width of table (optional)
	  columns: [ //Define Table Columns
	      { title: "Time", field: "time", hozAlign: "center"  },
	      { title: "Type", field: "type", hozAlign: "left", hozAlign: "center"  },
	      { title: "Subtype", field: "subtype", hozAlign: "center" },
	      { title: "Params", field: "params", hozAlign: "center" },
	  ],
      });
      
      
      //trigger an alert message when the row is clicked
      ess_table.on("rowClick", function (e, row) {
	  
      });
    </script>


    <script>
      var term = new window.Terminal({
	  cursorBlink: true,
	  rows: 12
      });
      
      var fitAddon = new window.FitAddon.FitAddon();
      var searchAddon = new window.SearchAddon.SearchAddon();
      
      term.open(document.getElementById('terminal'));
      term.loadAddon(fitAddon);
      term.loadAddon(searchAddon);
      
      fitAddon.fit();
      
      function r() {fitAddon.fit();}
      
      function init_term() {
	  if (term._initialized) {
	      return;
	  }
	  
	  $(window).resize(r);
	  
	  term._initialized = true;
	  
	  term.prompt = () => {
	      term.write('\r\n$ ');
	  };
	  prompt(term);
	  
	  function printResult(msg) {
	      console.log(msg)
	      if (msg.length > 3) {
		  let result = msg.replaceAll('\n','\r\n');
		  if (msg[0] == '1') // err
		      term.write('\033[91m'+result.slice(2)+'\033[0m');
		  else
		      term.write(result.slice(2));
	      }
	      term.write('$ ');
	  };
	  
	  function printPrint(dsmsg) {
	      let msg = dsmsg.split(" ").slice(4).join(' ').replace( /[{}]/g, '' )
	      if (msg.length > 0) {
		  term.write(msg);
		  term.write('\r\n');
	      }
	  };
	  
	  function printError(dsmsg) {
	      term.write('\r\n');
	      let msg = dsmsg.split(" ").slice(4).join(' ').replace( /[{}]/g, '' )
	      if (msg.length > 0) {
		  term.write('\033[91m'+msg+'\033[0m');
		  term.write('\r\n');
	      }
	      term.write('$ ');
	  };
	  
	  function printEvent(evtmsg) {
	      console.log(evtmsg);
	  }
	  
	  smq.subscribe("result", {onmsg:printResult, datatype:"text"});
	  smq.subscribe("print", {onmsg:printPrint, datatype:"text"});
	  smq.subscribe("error", {onmsg:printError, datatype:"text"});	  
	  
	  term.onData(e => {
	      switch (e) {
	      case '\u0003': // Ctrl+C
		  term.write('^C');
		  prompt(term);
		  break;
	      case '\r': // Enter
		  term.write('\r\n');
		  if (command.length > 0) {
		      runCommand(smq, command);
		  } else {
		      term.write('$ ');
		  }
		  command = '';
		  break;
	      case '\u007F': // Backspace (DEL)
		  // Do not delete the prompt
		  if (term._core.buffer.x > 2) {
		      term.write('\b \b');
		      if (command.length > 0) {
			  command = command.substr(0, command.length - 1);
		      }
		  }
		  break;
	      case '\u0009':
		  console.log('tabbed', output, ["dd", "ls"]);
		  break;
	      default:
		  if (e >= String.fromCharCode(0x20) && e <= String.fromCharCode(0x7E) || e >= '\u00a0') {
		      command += e;
		      term.write(e);
		  }
	      }
	  });
      }
      
      function clearInput(command) {
	  var inputLengh = command.length;
	  for (var i = 0; i < inputLengh; i++) {
	      term.write('\b \b');
	  }
      }
      function prompt(term) {
	  command = '';
	  term.write('\r\n$ ');
      }
      
      function runCommand(smq, command) {
	  if (command.length > 0) {
	      smq.publish(command, "xterm_command");
	      console.log(command + '\n');
	      return;
	  }
      }
      init_term();
    </script>
    <style>
      .wrapper {
	display: grid;
	grid-template-columns: 1fr, 1fr;
	grid-auto-rows: minmax(0px, auto);
	font-family: sans-serif
      }
      .system-selector {
	  display: flex;
	  flex-flow: row;
      }
      .system-selector > div {
	  padding: 1rem;
      }
      .system-selector-auto {
	  flex: auto;
	  border: 1px solid #f00;
      }
      
      .box1 {
	  grid-column: 1;
	  grid-row 3;
      }
      .box2 {
	  grid-column: 2;
	  grid-row 3;
      }
      .button-grp {
	  grid-column: 1;
	  grid-row 1;
      }
      .term {
	  grid-column: 1/3;
	  grid-row 2;
      }
      .btn-group button {
	  background-color: #04AA6D; /* Green background */
	  border: 1px solid green; /* Green border */
	  color: white; /* White text */
	  padding: 10px 24px; /* Some padding */
	  cursor: pointer; /* Pointer/hand icon */
	  float: left; /* Float the buttons side by side */
      }
      
      .btn-group button:not(:last-child) {
	  border-right: none; /* Prevent double borders */
      }
      
      /* Clear floats (clearfix hack) */
      .btn-group:after {
	  content: "";
	  clear: both;
	  display: table;
      }
      
      /* Add a background color on hover */
      .btn-group button:hover {
	  background-color: #3e8e41;
      }
    </style>
  </body>
</html>
