contract C {
    event E();
}

contract Test {
    function f() public {
        emit C.E();
    }
}
// ====
// compileViaYul: also
// ----
// f() ->
// ~ emit E()
