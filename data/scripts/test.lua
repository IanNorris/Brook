-- Brook OS Lua test script
print("=== LUA TEST on Brook OS ===")
print("Lua version: " .. _VERSION)

-- Arithmetic
local sum = 0
for i = 1, 100 do sum = sum + i end
print("Sum 1..100 = " .. sum)

-- String operations
local greeting = "Hello from Lua on Brook!"
print(greeting)
print("String length: " .. #greeting)

-- Tables
local fib = {1, 1}
for i = 3, 20 do
    fib[i] = fib[i-1] + fib[i-2]
end
print("fib(20) = " .. fib[20])

-- Functions
local function factorial(n)
    if n <= 1 then return 1 end
    return n * factorial(n - 1)
end
print("15! = " .. factorial(15))

-- Coroutines
local co = coroutine.create(function()
    for i = 1, 3 do
        coroutine.yield(i * i)
    end
end)
local squares = {}
for i = 1, 3 do
    local _, v = coroutine.resume(co)
    squares[i] = v
end
print("Squares: " .. table.concat(squares, ", "))

print("=== LUA TEST COMPLETE ===")
