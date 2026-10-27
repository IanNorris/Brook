(module
 (memory (export "memory") 1)
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (global (mut i64) (i64.const 7))
 (func (export "dispatch") (param i32 i32) (result i32)
  (block (loop (block
   local.get 1
   local.get 0
   i32.load offset=8 align=1
   i32.eq
   if
    i64.const 0
    global.set 16
    br 2
   end
   i32.const 42
   return
  )))
  i32.const 99)
)
