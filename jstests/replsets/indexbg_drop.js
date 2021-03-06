// Index drop race

var dbname = 'dropbgindex';
var collection = 'jstests_feh';
var size = 500000;

// Set up replica set
var replTest = new ReplSetTest({ name: 'bgIndex', nodes: 3 });
var nodes = replTest.nodeList();
printjson(nodes);

// We need an arbiter to ensure that the primary doesn't step down when we restart the secondary
replTest.startSet();
replTest.initiate({"_id" : "bgIndex",
                   "members" : [
                    {"_id" : 0, "host" : nodes[0]},
                    {"_id" : 1, "host" : nodes[1]},
                    {"_id" : 2, "host" : nodes[2], "arbiterOnly" : true}]});

var master = replTest.getMaster();
var second = replTest.getSecondary();

var masterId = replTest.getNodeId(master);
var secondId = replTest.getNodeId(second);

var masterDB = master.getDB(dbname);
var secondDB = second.getDB(dbname);


var dc = {dropIndexes: collection, index: "i_1"};

// set up collections
masterDB.dropDatabase();
jsTest.log("creating test data " + size + " documents");
for( i = 0; i < size; ++i ) {
	masterDB.getCollection(collection).save( {i: Random.rand()} );
}

jsTest.log("Starting background indexing for test of: " + tojson(dc));
masterDB.getCollection(collection).ensureIndex( {i:1}, {background:true} );
assert.eq(2, masterDB.system.indexes.count( {ns:dbname + "." + collection}, {background:true} ) );

// Wait for the secondary to get the index entry
assert.soon( 
    function() { return 2 == secondDB.system.indexes.count( {ns:dbname + "." + collection} ); }, 
    "index not created on secondary (prior to drop)", 120000, 50 );

jsTest.log("Index created and system.indexes entry exists on secondary");


// make sure the index build has started on secondary
assert.soon(function() {
    var curOp = secondDB.currentOp();
    printjson(curOp);
    for (var i=0; i < curOp.inprog.length; i++) {
        try {
            if (curOp.inprog[i].insert.background){

                return true; 
            }
        } catch (e) {
            // catchem if you can
        }
    }
    return false;
}, "waiting for secondary bg index build", 20000, 10);


jsTest.log("dropping index");
masterDB.runCommand( {dropIndexes: collection, index: "*"});
jsTest.log("Waiting on replication");
replTest.awaitReplication();

masterDB.system.indexes.find().forEach(printjson);
secondDB.system.indexes.find().forEach(printjson);

assert.eq(1, secondDB.system.indexes.count());

replTest.stopSet();
