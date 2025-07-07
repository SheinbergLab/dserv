<form>
  <p>dserv Variable:
     <input type="text" name="msg">
     <input type="submit" value="Submit">
  </p>
</form>
<?lsp
local varname=request:data"msg"
if varname then
   local s,err = ba.socket.connect("localhost", 4620)
   if s then
      local d
      s:write("%get " .. varname)
      d,err = s:read(5000) -- Wait a maximum of 5 seconds for response
      print(d)
      s:close()
   end
   if err then
      print(err)
   end
end
?>

