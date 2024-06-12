<!doctype html>
  <html>
    <head>
      <link rel="stylesheet" href="node_modules/xterm/css/xterm.css" />
      <script src="node_modules/xterm/lib/xterm.js"></script>
      <script src="node_modules/xterm-addon-fit/lib/xterm-addon-fit.js"></script>
      <script src="node_modules/xterm-addon-search/lib/xterm-addon-search.js"></script>
      <script type="text/javascript" src="/rtl/jquery.js"></script>
      <script type="text/javascript" src="/rtl/smq.js"></script> 
    </head>
    <body>
      <div id="terminal"></div>
    </body>
    <script>
      var term = new window.Terminal({
	  cursorBlink: true
      });

      var fitAddon = new window.FitAddon.FitAddon();
      var searchAddon = new window.SearchAddon.SearchAddon();
      
      term.open(document.getElementById('terminal'));
      term.loadAddon(fitAddon);
      term.loadAddon(searchAddon);

      fitAddon.fit();
      
      function r() {fitAddon.fit();}
      
      function init() {
	  if (term._initialized) {
              return;
	  }

	  $(window).resize(r);
	  
	  var smq = SMQ.Client(SMQ.wsURL("/QPCSBroker/"));
	  
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
	  smq.subscribe("eventlog", {onmsg:printEvent, datatype:"text"});
	  
	  
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
      
      init();
    </script>
  </html>
