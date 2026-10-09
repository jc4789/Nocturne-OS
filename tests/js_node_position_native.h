/* Include after external_case helpers. Add RUN("node-position",test_node_position). */
static void test_node_position(void){
    external_case("js_dom_compare_cases.js",
        ";check('node-compare-legacy-count',runDOMCompareCases()===14);"
        "const positionChecks=runDOMPositionCases();console.log('Node position API checks '+positionChecks);"
        "check('node-position-count',positionChecks>=90);mark('api-done');",BASE);
}
