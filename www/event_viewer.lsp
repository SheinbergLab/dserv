<html>
  <head>
    <script src="/rtl/jquery.js"></script>
    <script src="/rtl/smq.js"></script>
    <script>
$(function() {
    function print(txt) {
        $("#console").append(txt+"\n");
    };
    var smq = SMQ.Client(SMQ.wsURL("/QPCSBroker/"));
    function serverMessage(msg) {
        print(msg);
    };
    smq.subscribe("eventlog", {onmsg:serverMessage, datatype:"text"});
});
    </script>
  </head>
  <body>
  <h1>Event Viewer:</h1>
  <pre id="console"></pre>
  </body>
</html>
