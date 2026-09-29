/*
    Validator registry is a contract that holds certain data for validators.
    Each validator can modify its own registry entry using a signed external message.
 */
import "@stdlib/gas-payments"

const VAL_REGISTRY_CONFIG_PARAM = 46;
const VALIDATOR_REQUEST_PERIOD = 120;
const ADNL_ID_REQUEST_PERIOD = 600;

const OP_CLEANUP = 0x32b85a9b;
const OP_UPDATE_CODE = 0x34bdaa6c;
const OP_CLEANUP_ADNL_ID = 0x7b2f3bcc;
const OP_REQUEST = 0xcdb9d286;

const ERR_INVALID_OP = 900;
const ERR_TOO_MANY_COLLATORS = 901;
const ERR_TOO_EARLY_CLEANUP = 902;
const ERR_WRONG_KEY_BLOCK_SEQNO = 903;
const ERR_WRONG_VAL_SET_IDX = 904;
const ERR_TOO_BIG_VALID_UNTIL = 905;
const ERR_MESSAGE_EXPIRED = 906;
const ERR_VALIDATOR_NOT_FOUND = 907;
const ERR_TOO_EARLY_REQUEST = 908;
const ERR_WRONG_SIGNATURE = 909;
const ERR_RANGE_ERROR = 910;
const ERR_NO_CONFIG = 911;
const ERR_BAD_NEW_CODE_HASH = 912;
const ERR_NEW_CODE_ALREADY_INSTALLED = 913;
const ERR_ADNL_ID_NOT_FOUND = 914;
const ERR_INVALID_ADNL_NODE = 915;
const ERR_ADNL_ID_IS_ALIVE = 916;

const OLD_STORAGE_TAG = 0x9bc722a8;

contract ValidatorRegistry {
    storage: StorageAny
}

struct (0x9bc722a8) StorageOld {
    registry: cell?,
    lastCleanupKeyBlockSeqno: uint32 = 0
}

struct (0x9bc722a9) Storage {
    registry: map<uint256, ValidatorInfo> = [],
    lastCleanupKeyBlockSeqno: uint32 = 0,
    adnlIds: map<uint256, AdnlIdInfo> = []
}

type StorageAny = Storage | StorageOld;

struct (0x57766eec) ValidatorInfo {
    allowUpdateAt: uint32 = 0,
    entry: Cell<Entry>? = null
}

struct (0xeae0b274) Entry {
    collators: map<uint256, Collator>,
    monitoringShards: ShardSet,
    selfAdnlId: uint256
}

struct (0x10364494) Collator {
    monitoringShards: ShardSet
}

struct (0x14108b04) AdnlIdInfo {
    allowUpdateAt: uint32 = 0,
    refcnt: uint32 = 0,
    adnlNode: cell? = null  // contains serialized adnl.node tl
}

struct (0xfbd82b02) RequestUpdate {
    lastKeyBlockSeqno: uint32,
    valSetIdx: uint8,
    valIdx: uint16,
    validUntil: uint32,
    newEntry: Cell<Entry>?
}

struct (0x498cf8c7) RequestUpdateAdnlId {
    adnlIdFull: uint256,
    validUntil: uint32,
    newAdnlNode: cell?
}

type RequestAny = RequestUpdate | RequestUpdateAdnlId;

struct ShardSet {
    // val_registry_shard_set$_ n:(#<= 8) mask:(BitsPow2 n) = ValRegistryShardSet;
    n: uint8,
    mask: int
}

fun ShardSet.packToBuilder(self, mutate b: builder) {
    b.storeUint(self.n, 4).storeUint(self.mask, 1 << self.n);
}

fun ShardSet.unpackFromSlice(mutate s: slice) {
    val n = s.loadUint(4);
    assert (n <= 8) throw ERR_RANGE_ERROR;
    val mask = s.loadUint(1 << n);
    return ShardSet{
        n, mask
    };
}

struct (0x3601163e) RegistryConfig {
    addr: uint256,
    maxCollatorsPerValidator: uint32,
    newCodeHash: uint256?
}

fun loadStorage(): Storage {
    val storage = StorageAny.fromCell(contract.getData());
    match (storage) {
        Storage => {
            return storage;
        }
        StorageOld => {
            return Storage{
                lastCleanupKeyBlockSeqno: storage.lastCleanupKeyBlockSeqno
            };
        }
    }
}

@pure
fun getLastKeyBlockSeqno(): uint32
    asm "PREVKEYBLOCK 2 INDEX";

fun getRegistryConfigCell(): cell {
    var c = blockchain.configParam(VAL_REGISTRY_CONFIG_PARAM);
    assert (c != null) throw ERR_NO_CONFIG;
    return c;
}

fun validateEntry(c: Cell<Entry>?) {
    if (c == null) {
        return;
    }
    val entry = c.load();
    val config = lazy RegistryConfig.fromCell(getRegistryConfigCell());
    val maxCollators = config.maxCollatorsPerValidator;
    var nCollators = 0;
    var r = entry.collators.findFirst();
    while (r.isFound) {
        nCollators += 1;
        assert (nCollators <= maxCollators) throw ERR_TOO_MANY_COLLATORS;
        r.loadValue();
        r = entry.collators.iterateNext(r);
    }
}

fun getValidatorKeys(configIdx: int, mutate validators: map<uint256, ()>) {
    val param = blockchain.configParam(configIdx);
    if (param == null) {
        return;
    }
    val paramCs = param.beginParse();
    if (paramCs.remainingRefsCount() == 0) {
        return;
    }

    val valSet: map<uint16, slice> = createMapFromLowLevelDict(paramCs.preloadRef());
    var r = valSet.findFirst();
    while (r.isFound) {
        // ed25519_pubkey#8e81278a pubkey:bits256 = SigPubKey;
        // validator#53 public_key:SigPubKey weight:uint64 = ValidatorDescr;
        // validator_addr#73 public_key:SigPubKey weight:uint64 adnl_addr:bits256 = ValidatorDescr;
        var cs = r.loadValue();
        cs.skipBits(8 + 32);
        validators.set(cs.loadUint(256), ());
        r = valSet.iterateNext(r);
    }
}

fun updateAdnlIdRefcnt(mutate storage: Storage, adnlId: uint256, diff: int) {
    val r = storage.adnlIds.get(adnlId);
    var info: AdnlIdInfo;
    if (r.isFound) {
        info = r.loadValue();
    } else {
        info = AdnlIdInfo{
        };
    }
    info.refcnt += diff;
    if (info.refcnt == 0 && blockchain.now() >= info.allowUpdateAt) {
        storage.adnlIds.delete(adnlId);
    } else {
        storage.adnlIds.set(adnlId, info);
    }
}

fun updateAdnlIdRefcntForEntry(mutate storage: Storage, entry: Entry, diff: int) {
    updateAdnlIdRefcnt(mutate storage, entry.selfAdnlId, diff);
    var r = entry.collators.findFirst();
    while (r.isFound) {
        updateAdnlIdRefcnt(mutate storage, r.getKey(), diff);
        r = entry.collators.iterateNext(r);
    }
}

fun updateAdnlIdRefcntForEntryRef(mutate storage: Storage, entry: Cell<Entry>?, diff: int) {
    if (entry != null) {
        updateAdnlIdRefcntForEntry(mutate storage, entry.load(), diff);
    }
}

fun cleanupRegistry() {
    var storage = loadStorage();
    val lastKeyBlockSeqno = getLastKeyBlockSeqno();
    assert (storage.lastCleanupKeyBlockSeqno < lastKeyBlockSeqno) throw ERR_TOO_EARLY_CLEANUP;
    acceptExternalMessage();

    var validators: map<uint256, ()> = [];
    getValidatorKeys(32, mutate validators);
    getValidatorKeys(34, mutate validators);
    getValidatorKeys(36, mutate validators);

    var r = storage.registry.findFirst();
    var iter = 0;
    while (r.isFound) {
        val key = r.getKey();
        if (!validators.exists(key)) {
            updateAdnlIdRefcntForEntryRef(mutate storage, r.loadValue().entry, -1);
            storage.registry.delete(key);
        }
        r = storage.registry.iterateNext(r);
        iter += 1;
        if (iter % 10 == 0) {
            // Save intermediate storage to avoid failure when resistry wasn't cleaned for a long time so
            // special_gas_limit is not enough to process the whole dict.
            // Cleanup will finish eventually because the number of keys in 32/34/36 sets is limited.
            contract.setData(storage.toCell());
            commitContractDataAndActions();
        }
    }
    storage.lastCleanupKeyBlockSeqno = lastKeyBlockSeqno;
    contract.setData(storage.toCell());
}

fun updateCode(newCode: cell) {
    val config = lazy RegistryConfig.fromCell(getRegistryConfigCell());
    val newCodeHash = config.newCodeHash;
    assert (newCodeHash != null && newCodeHash == newCode.hash()) throw ERR_BAD_NEW_CODE_HASH;
    assert (contract.getCode().hash() != newCodeHash) throw ERR_NEW_CODE_ALREADY_INSTALLED;
    contract.setCodePostponed(newCode);
    acceptExternalMessage();
}

fun cleanupAdnlId(adnlId: uint256) {
    var storage = loadStorage();
    val r = storage.adnlIds.get(adnlId);
    assert (r.isFound) throw ERR_ADNL_ID_NOT_FOUND;
    val info = r.loadValue();
    assert (info.refcnt == 0 && blockchain.now() >= info.allowUpdateAt) throw ERR_ADNL_ID_IS_ALIVE;
    acceptExternalMessage();
    storage.adnlIds.delete(adnlId);
    contract.setData(storage.toCell());
}

fun getValidatorPublicKey(valSetIdx: int, valIdx: uint16): uint256 {
    val param = blockchain.configParam(valSetIdx);
    assert (param != null) throw ERR_VALIDATOR_NOT_FOUND;
    val paramCs = param.beginParse();
    assert (paramCs.remainingRefsCount() > 0) throw ERR_VALIDATOR_NOT_FOUND;
    val valSet: map<uint16, slice> = createMapFromLowLevelDict(paramCs.preloadRef());
    val r = valSet.get(valIdx);
    assert (r.isFound) throw ERR_VALIDATOR_NOT_FOUND;
    // ed25519_pubkey#8e81278a pubkey:bits256 = SigPubKey;
    // validator#53 public_key:SigPubKey weight:uint64 = ValidatorDescr;
    // validator_addr#73 public_key:SigPubKey weight:uint64 adnl_addr:bits256 = ValidatorDescr;
    var cs = r.loadValue();
    cs.skipBits(8 + 32);
    return cs.loadUint(256);
}

fun onRequestUpdate(signature: slice, toSignHash: uint256, request: RequestUpdate) {
    assert (request.lastKeyBlockSeqno == getLastKeyBlockSeqno()) throw ERR_WRONG_KEY_BLOCK_SEQNO;
    assert (request.valSetIdx == 32 || request.valSetIdx == 34 || request.valSetIdx == 36) throw ERR_WRONG_VAL_SET_IDX;
    assert (request.validUntil < blockchain.now() + VALIDATOR_REQUEST_PERIOD) throw ERR_TOO_BIG_VALID_UNTIL;
    assert (request.validUntil > blockchain.now()) throw ERR_MESSAGE_EXPIRED;

    val publicKey = getValidatorPublicKey(request.valSetIdx, request.valIdx);
    var storage = loadStorage();
    val r = storage.registry.get(publicKey);
    var info: ValidatorInfo;
    if (r.isFound) {
        info = r.loadValue();
    } else {
        info = ValidatorInfo{
        };
    }
    assert (blockchain.now() >= info.allowUpdateAt) throw ERR_TOO_EARLY_REQUEST;

    assert (isSignatureValid(toSignHash, signature, publicKey)) throw ERR_WRONG_SIGNATURE;

    acceptExternalMessage();
    info.allowUpdateAt = blockchain.now() + VALIDATOR_REQUEST_PERIOD;
    storage.registry.set(publicKey, info);
    contract.setData(storage.toCell());
    commitContractDataAndActions();

    validateEntry(request.newEntry);
    updateAdnlIdRefcntForEntryRef(mutate storage, request.newEntry, 1);
    updateAdnlIdRefcntForEntryRef(mutate storage, info.entry, -1);
    info.entry = request.newEntry;
    storage.registry.set(publicKey, info);
    contract.setData(storage.toCell());
}

fun onRequestUpdateAdnlId(signature: slice, toSignHash: uint256, request: RequestUpdateAdnlId) {
    assert (request.validUntil < blockchain.now() + ADNL_ID_REQUEST_PERIOD) throw ERR_TOO_BIG_VALID_UNTIL;
    assert (request.validUntil > blockchain.now()) throw ERR_MESSAGE_EXPIRED;
    val adnlIdShort = beginCell().storeUint(0xc6b41348, 32).storeUint(request.adnlIdFull, 256)
        .endCell().beginParse().bitsHash();

    var storage = loadStorage();
    val r = storage.adnlIds.get(adnlIdShort);
    assert (r.isFound) throw ERR_ADNL_ID_NOT_FOUND;
    var info: AdnlIdInfo = r.loadValue();
    assert (info.refcnt > 0) throw ERR_ADNL_ID_NOT_FOUND;
    assert (blockchain.now() >= info.allowUpdateAt) throw ERR_TOO_EARLY_REQUEST;

    assert (isSignatureValid(toSignHash, signature, request.adnlIdFull)) throw ERR_WRONG_SIGNATURE;
    if (request.newAdnlNode != null) {
        val cs = request.newAdnlNode.beginParse();
        assert (cs.remainingRefsCount() == 0 && cs.remainingBitsCount() % 8 == 0) throw ERR_INVALID_ADNL_NODE;
    }

    acceptExternalMessage();
    info.allowUpdateAt = blockchain.now() + ADNL_ID_REQUEST_PERIOD;
    info.adnlNode = request.newAdnlNode;
    storage.adnlIds.set(adnlIdShort, info);
    contract.setData(storage.toCell());
}

fun onInternalMessage() {
}

fun onExternalMessage(inMsg: slice) {
    val opcode = inMsg.loadUint(32);
    if (opcode == OP_CLEANUP) {
        if (!inMsg.isEmpty()) {
            inMsg.skipBits(64);
            inMsg.assertEnd();
        }
        cleanupRegistry();
        return;
    }
    if (opcode == OP_UPDATE_CODE) {
        val newCode = inMsg.loadRef();
        inMsg.assertEnd();
        updateCode(newCode);
        return;
    }
    if (opcode == OP_CLEANUP_ADNL_ID) {
        val adnlId = inMsg.loadUint(256);
        inMsg.skipBits(64);
        inMsg.assertEnd();
        cleanupAdnlId(adnlId);
        return;
    }

    assert (opcode == OP_REQUEST) throw ERR_INVALID_OP;
    val signature = inMsg.loadBits(512);
    val toSign = inMsg.hash();
    val request = RequestAny.fromSlice(inMsg);
    match (request) {
        RequestUpdate => {
            onRequestUpdate(signature, toSign, request)
        }
        RequestUpdateAdnlId => {
            onRequestUpdateAdnlId(signature, toSign, request)
        }
    }
}