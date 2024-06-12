<form>
  <p>Stim command:
     <input type="text" name="msg">
     <input type="submit" value="Submit">
  </p>
</form>
<?lsp
local msg=request:data"msg"
if msg then
   local s,err = ba.socket.connect("localhost", 4610)
   if s then
      local d
      s:write(msg)
      d,err = s:read(5000) -- Wait a maximum of 5 seconds for response
      print(d)
      s:close()
   end
   if err then
      print(err)
   end
end
?>

