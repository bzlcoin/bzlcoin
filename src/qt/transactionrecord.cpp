#include "transactionrecord.h"

#include "wallet.h"
#include "base58.h"

/* Return positive answer if transaction should be shown in list.
 */
bool TransactionRecord::showTransaction(const CWalletTx &wtx)
{
    if (wtx.IsCoinBase())
    {
        // Ensures we show generated coins / mined transactions at depth 1
        if (!wtx.IsInMainChain())
        {
            return false;
        }
    }
    return true;
}

/*
 * Decompose CWallet transaction to model transaction records.
 */
QList<TransactionRecord> TransactionRecord::decomposeTransaction(const CWallet *wallet, const CWalletTx &wtx)
{
    QList<TransactionRecord> parts;
    int64_t nTime = wtx.GetTxTime();
    int64_t nCredit = wtx.GetCredit(ISMINE_ALL); //Updated Watch Only
    int64_t nDebit = wtx.GetDebit(ISMINE_ALL);
    int64_t nNet = nCredit - nDebit;
    uint256 hash = wtx.GetHash(), hashPrev = 0;
    std::map<std::string, std::string> mapValue = wtx.mapValue;
    
    char cbuf[256];
    
    if (nNet > 0 || wtx.IsCoinBase() || wtx.IsCoinStake())
    {
        //
        // Credit
        //
        
        for (unsigned int nOut = 0; nOut < wtx.vout.size(); nOut++)
        {
            const CTxOut& txout = wtx.vout[nOut];

            isminetype mine = wallet->IsMine(txout);
            if(mine)
            {
                TransactionRecord sub(hash, nTime);
                CTxDestination address;
                sub.idx = parts.size(); // sequence number
                sub.credit = txout.nValue;
                sub.involvesWatchAddress = mine == ISMINE_WATCH_ONLY;
                if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*wallet, address))
                {
                    // Received by Bitcoin Address
                    sub.type = TransactionRecord::RecvWithAddress;
                    sub.address = CBitcoinAddress(address).ToString();
                }
                else
                {
                    // Received by IP connection (deprecated features), or a multisignature or other non-simple transaction
                    sub.type = TransactionRecord::RecvFromOther;
                    sub.address = mapValue["from"];
                }
                
                snprintf(cbuf, sizeof(cbuf), "n_%u", nOut);
                mapValue_t::const_iterator mi = wtx.mapValue.find(cbuf);
                if (mi != wtx.mapValue.end() && !mi->second.empty())
                    sub.narration = mi->second;
                
                if (wtx.IsCoinBase())
                {
                    // Generated (proof-of-work)
                    sub.type = TransactionRecord::Generated;
                }
                if (wtx.IsCoinStake())
                {
                    // Generated (proof-of-stake)

                    if (hashPrev == hash)
                        continue; // last coinstake output
                    CAmount nValueOut = 0;
                    BOOST_FOREACH(const CTxOut& txout, wtx.vout)
                    {
                        if (IsMine(*wallet,txout.scriptPubKey))
                            nValueOut += txout.nValue;
                        if (!MoneyRange(txout.nValue) || !MoneyRange(nValueOut))
                            throw std::runtime_error("CTransaction::GetValueOut() : value out of range");
                    }
                    sub.type = TransactionRecord::Generated;
                    sub.credit = nNet > 0 ? nNet : nValueOut - nDebit;
                    hashPrev = hash;
                }

                parts.append(sub);
            }
        }
    }
    else
    {
        bool involvesWatchAddress = false;
        isminetype fAllFromMe = ISMINE_SPENDABLE;
        BOOST_FOREACH(const CTxIn& txin, wtx.vin)
        {
            isminetype mine = wallet->IsMine(txin);
            if(mine == ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if(fAllFromMe > mine) fAllFromMe = mine;
        }

        isminetype fAllToMe = ISMINE_SPENDABLE;
        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
        {
            isminetype mine = wallet->IsMine(txout);
            if(mine == ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if(fAllToMe > mine) fAllToMe = mine;
        }

        if (fAllFromMe && fAllToMe)
        {
            // Payment to self
            int64_t nChange = wtx.GetChange();
            
            std::string narration("");
            mapValue_t::const_iterator mi;
            for (mi = wtx.mapValue.begin(); mi != wtx.mapValue.end(); ++mi)
            {
                if (mi->first.compare(0, 2, "n_") != 0)
                    continue;
                narration = mi->second;
                break;
            };
            
            parts.append(TransactionRecord(hash, nTime, TransactionRecord::SendToSelf, "", narration,
                            -(nDebit - nChange), nCredit - nChange));
            parts.last().involvesWatchAddress = involvesWatchAddress;   // maybe pass to TransactionRecord as constructor argument
        }
        else if (fAllFromMe)
        {
            //
            // Debit
            //
            int64_t nTxFee = nDebit - wtx.GetValueOut();
            
            
            for (unsigned int nOut = 0; nOut < wtx.vout.size(); nOut++)
            {
                const CTxOut& txout = wtx.vout[nOut];
                TransactionRecord sub(hash, nTime);
                sub.idx = parts.size();
                sub.involvesWatchAddress = involvesWatchAddress;

                opcodetype firstOpCode;
                CScript::const_iterator pc = txout.scriptPubKey.begin();
                if (txout.scriptPubKey.GetOp(pc, firstOpCode)
                    && firstOpCode == OP_RETURN)
                    continue;

                if (wallet->IsMine(txout))
                {
                    // Ignore parts sent to self, as this is usually the change
                    // from a transaction sent back to our own address.
                    continue;
                }

                CTxDestination address;
                if (ExtractDestination(txout.scriptPubKey, address))
                {
                    // Sent to Bitcoin Address
                    sub.type = TransactionRecord::SendToAddress;
                    sub.address = CBitcoinAddress(address).ToString();
                }
                else
                {
                    // Sent to IP, or other non-address transaction like OP_EVAL
                    sub.type = TransactionRecord::SendToOther;
                    sub.address = mapValue["to"];
                }
                
                snprintf(cbuf, sizeof(cbuf), "n_%u", nOut);
                mapValue_t::const_iterator mi = wtx.mapValue.find(cbuf);
                if (mi != wtx.mapValue.end() && !mi->second.empty())
                    sub.narration = mi->second;

                int64_t nValue = txout.nValue;
                /* Add fee to first output */
                if (nTxFee > 0)
                {
                    nValue += nTxFee;
                    nTxFee = 0;
                }
                sub.debit = -nValue;

                parts.append(sub);
            }
        }
        else
        {
            //
            // Mixed debit transaction, can't break down payees
            //
            parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", "", nNet, 0));
            parts.last().involvesWatchAddress = involvesWatchAddress;
        }
    }

    return parts;
}

void TransactionRecord::updateStatus(const CWalletTx &wtx)
{
    AssertLockHeld(cs_main);
    // Determine transaction status

    // Find the block the tx is in
    CBlockIndex* pindex = NULL;
    std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(wtx.hashBlock);
    if (mi != mapBlockIndex.end())
        pindex = (*mi).second;

    // Sort order, unrecorded transactions sort to the top
    status.sortKey = strprintf("%010d-%01d-%010u-%03d",
        (pindex ? pindex->nHeight : std::numeric_limits<int>::max()),
        (wtx.IsCoinBase() ? 1 : 0),
        wtx.nTimeReceived,
        idx);
    status.countsForBalance = wtx.IsTrusted() && !(wtx.GetBlocksToMaturity() > 0);
    status.depth = wtx.GetDepthInMainChain();
    status.cur_num_blocks = nBestHeight;

    if (!wtx.IsFinal())
    {
        if (wtx.nLockTime < LOCKTIME_THRESHOLD)
        {
            status.status = TransactionStatus::OpenUntilBlock;
            status.open_for = nBestHeight - wtx.nLockTime;
        }
        else
        {
            status.status = TransactionStatus::OpenUntilDate;
            status.open_for = wtx.nLockTime;
        }
    }

    // For generated transactions, determine maturity
    else if(type == TransactionRecord::Generated)
    {
        if (wtx.GetBlocksToMaturity() > 0)
        {
            status.status = TransactionStatus::Immature;

            if (wtx.IsInMainChain())
            {
                status.matures_in = wtx.GetBlocksToMaturity();

                // Check if the block was requested by anyone
                if (GetAdjustedTime() - wtx.nTimeReceived > 2 * 60 && wtx.GetRequestCount() == 0)
                    status.status = TransactionStatus::MaturesWarning;
            }
            else
            {
                status.status = TransactionStatus::NotAccepted;
            }
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    else
    {
        if (status.depth < 0)
        {
            status.status = TransactionStatus::Conflicted;
        }
        else if (GetAdjustedTime() - wtx.nTimeReceived > 2 * 60 && wtx.GetRequestCount() == 0)
        {
            status.status = TransactionStatus::Offline;
        }
        else if (status.depth == 0)
        {
            status.status = TransactionStatus::Unconfirmed;
        }
        else if (status.depth < RecommendedNumConfirmations)
        {
            status.status = TransactionStatus::Confirming;
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
}

bool TransactionRecord::statusUpdateNeeded()
{
    AssertLockHeld(cs_main);
    return status.cur_num_blocks != nBestHeight;
}

std::string TransactionRecord::getTxID()
{
    return hash.ToString() + strprintf("-%03d", idx);
}

