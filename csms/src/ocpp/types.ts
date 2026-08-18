/**
 * OCPP 1.6J wire types.
 *
 * Payloads are validated with zod at the boundary rather than trusted. A charge
 * point is a field device on a network we do not control; a malformed or hostile
 * StopTransaction should be rejected with a CALLERROR, not written to the
 * billing table.
 */
import { z } from 'zod';

export const MessageType = {
  CALL: 2,
  CALLRESULT: 3,
  CALLERROR: 4,
} as const;

/** OCPP-J error codes we actually emit. */
export const OcppError = {
  NOT_IMPLEMENTED: 'NotImplemented',
  NOT_SUPPORTED: 'NotSupported',
  INTERNAL_ERROR: 'InternalError',
  PROTOCOL_ERROR: 'ProtocolError',
  SECURITY_ERROR: 'SecurityError',
  FORMATION_VIOLATION: 'FormationViolation',
  PROPERTY_CONSTRAINT_VIOLATION: 'PropertyConstraintViolation',
  TYPE_CONSTRAINT_VIOLATION: 'TypeConstraintViolation',
  GENERIC_ERROR: 'GenericError',
} as const;

/* OCPP 1.6 CiString limits. Enforcing them matters: a charge point that sends
 * an over-length idTag is either broken or probing, and either way it must not
 * end up in the database. */
const CiString20 = z.string().max(20);
const CiString25 = z.string().max(25);
const CiString50 = z.string().max(50);
const CiString255 = z.string().max(255);

export const IdTagInfoSchema = z.object({
  status: z.enum(['Accepted', 'Blocked', 'Expired', 'Invalid', 'ConcurrentTx']),
  expiryDate: z.string().optional(),
  parentIdTag: CiString20.optional(),
});
export type IdTagInfo = z.infer<typeof IdTagInfoSchema>;

export const BootNotificationSchema = z.object({
  chargePointVendor: CiString20,
  chargePointModel: CiString20,
  chargePointSerialNumber: CiString25.optional(),
  chargeBoxSerialNumber: CiString25.optional(),
  firmwareVersion: CiString50.optional(),
  iccid: CiString20.optional(),
  imsi: CiString20.optional(),
  meterType: CiString25.optional(),
  meterSerialNumber: CiString25.optional(),
});

export const HeartbeatSchema = z.object({}).passthrough();

export const StatusNotificationSchema = z.object({
  connectorId: z.number().int().min(0),
  errorCode: z.enum([
    'ConnectorLockFailure', 'EVCommunicationError', 'GroundFailure',
    'HighTemperature', 'InternalError', 'LocalListConflict', 'NoError',
    'OtherError', 'OverCurrentFailure', 'OverVoltage', 'PowerMeterFailure',
    'PowerSwitchFailure', 'ReaderFailure', 'ResetFailure', 'UnderVoltage',
    'WeakSignal',
  ]),
  status: z.enum([
    'Available', 'Preparing', 'Charging', 'SuspendedEVSE', 'SuspendedEV',
    'Finishing', 'Reserved', 'Unavailable', 'Faulted',
  ]),
  timestamp: z.string().optional(),
  info: CiString50.optional(),
  vendorId: CiString255.optional(),
  vendorErrorCode: CiString50.optional(),
});

export const AuthorizeSchema = z.object({ idTag: CiString20 });

export const StartTransactionSchema = z.object({
  connectorId: z.number().int().min(1),
  idTag: CiString20,
  meterStart: z.number().int(),
  reservationId: z.number().int().optional(),
  timestamp: z.string(),
});

export const StopTransactionSchema = z.object({
  transactionId: z.number().int(),
  idTag: CiString20.optional(),
  meterStop: z.number().int(),
  timestamp: z.string(),
  reason: z.enum([
    'EmergencyStop', 'EVDisconnected', 'HardReset', 'Local', 'Other',
    'PowerLoss', 'Reboot', 'Remote', 'SoftReset', 'UnlockCommand', 'DeAuthorized',
  ]).optional(),
  transactionData: z.array(z.any()).optional(),
});

export const SampledValueSchema = z.object({
  value: z.string(),
  context: z.string().optional(),
  format: z.string().optional(),
  measurand: z.string().optional(),
  phase: z.string().optional(),
  location: z.string().optional(),
  unit: z.string().optional(),
});

export const MeterValuesSchema = z.object({
  connectorId: z.number().int().min(0),
  transactionId: z.number().int().optional(),
  meterValue: z.array(z.object({
    timestamp: z.string(),
    sampledValue: z.array(SampledValueSchema),
  })),
});

export const DataTransferSchema = z.object({
  vendorId: CiString255,
  messageId: CiString50.optional(),
  data: z.string().optional(),
});

export const DiagnosticsStatusSchema = z.object({
  status: z.enum(['Idle', 'Uploaded', 'UploadFailed', 'Uploading']),
});

export const FirmwareStatusSchema = z.object({
  status: z.enum([
    'Downloaded', 'DownloadFailed', 'Downloading', 'Idle',
    'InstallationFailed', 'Installing', 'Installed',
  ]),
});

/** Actions a charge point may send to us. */
export const INBOUND_SCHEMAS = {
  BootNotification: BootNotificationSchema,
  Heartbeat: HeartbeatSchema,
  StatusNotification: StatusNotificationSchema,
  Authorize: AuthorizeSchema,
  StartTransaction: StartTransactionSchema,
  StopTransaction: StopTransactionSchema,
  MeterValues: MeterValuesSchema,
  DataTransfer: DataTransferSchema,
  DiagnosticsStatusNotification: DiagnosticsStatusSchema,
  FirmwareStatusNotification: FirmwareStatusSchema,
} as const;

export type InboundAction = keyof typeof INBOUND_SCHEMAS;

/** Actions we may send to a charge point. */
export type OutboundAction =
  | 'RemoteStartTransaction' | 'RemoteStopTransaction' | 'Reset'
  | 'UnlockConnector' | 'ChangeConfiguration' | 'GetConfiguration'
  | 'ChangeAvailability' | 'ClearCache' | 'TriggerMessage'
  | 'SetChargingProfile' | 'ClearChargingProfile' | 'GetCompositeSchedule'
  | 'SendLocalList' | 'GetLocalListVersion'
  | 'ReserveNow' | 'CancelReservation'
  | 'UpdateFirmware' | 'GetDiagnostics' | 'DataTransfer';

/** Parse a raw frame into its parts, or throw with a protocol error. */
export function parseFrame(raw: string): {
  type: number;
  id: string;
  action?: string;
  payload?: unknown;
  errorCode?: string;
  errorDescription?: string;
} {
  let msg: unknown;
  try {
    msg = JSON.parse(raw);
  } catch {
    throw new OcppFrameError(OcppError.PROTOCOL_ERROR, 'Body is not valid JSON');
  }
  if (!Array.isArray(msg) || msg.length < 3) {
    throw new OcppFrameError(OcppError.PROTOCOL_ERROR, 'Frame must be an array of at least 3 elements');
  }

  const [type, id] = msg as [unknown, unknown];
  if (typeof type !== 'number' || typeof id !== 'string') {
    throw new OcppFrameError(OcppError.PROTOCOL_ERROR, 'Bad message type or id');
  }

  if (type === MessageType.CALL) {
    const action = msg[2];
    if (typeof action !== 'string') {
      throw new OcppFrameError(OcppError.PROTOCOL_ERROR, 'CALL action must be a string');
    }
    return { type, id, action, payload: msg[3] ?? {} };
  }
  if (type === MessageType.CALLRESULT) {
    return { type, id, payload: msg[2] ?? {} };
  }
  if (type === MessageType.CALLERROR) {
    return {
      type, id,
      errorCode: typeof msg[2] === 'string' ? msg[2] : 'GenericError',
      errorDescription: typeof msg[3] === 'string' ? msg[3] : '',
      payload: msg[4] ?? {},
    };
  }
  throw new OcppFrameError(OcppError.PROTOCOL_ERROR, `Unknown message type ${type}`);
}

/** Thrown when a frame cannot be interpreted; carries the code to reply with. */
export class OcppFrameError extends Error {
  constructor(public readonly code: string, message: string) {
    super(message);
    this.name = 'OcppFrameError';
  }
}

export function ocppNow(): string {
  return new Date().toISOString();
}
