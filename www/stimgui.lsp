<!DOCTYPE html>
<html>
<head>
<style>

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
</head>
<body>


<div class="header">
  <h1>Stimgui</h1>
</div>

<script src="/rtl/jquery.js"></script>
<script src="/rtl/smq.js"></script>

<script>
var smq = SMQ.Client(SMQ.wsURL("/QPCSBroker/"));
function user_func (action) {
    smq.publish(action, "action");
    console.log(action)
};
</script>

<div class="btn-group">
  <button onclick="user_func('USER_START')" id="go_button" name="go">Go</button>
  <button onclick="user_func('USER_STOP')" id="stop_button" name="stop">Stop</button>
  <button onclick="user_func('USER_RESET')" id="reset_button" name="reset">Reset</button>
</div>

</body>
</html>

